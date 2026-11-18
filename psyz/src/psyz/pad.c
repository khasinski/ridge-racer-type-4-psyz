/* Controller frames and the byte-level answers of the emulated PS1
 * controllers.
 *
 * The platform layer samples the host (buttons, sticks, triggers) into a
 * PsyzPadInput; Psyz_PadsBuildFrame turns it into the frame the controller
 * kind sends when it is polled (command 0x42), byte for byte as on the
 * console:
 *   https://psx-spx.consoledev.net/controllersandmemorycards/
 *
 * Analog inputs that the host only approximates (a stick for the neGcon
 * twist or the JogCon dial, triggers for the neGcon's analog buttons) go
 * through a response curve with DuckStation's parameters (dead zone,
 * saturation, linearity, scaling), so values tuned there carry over.
 *
 * libpad (libpad.c) talks to the controllers a byte at a time
 * (Psyz_PadsTransfer), and they answer as DuckStation's do
 * (digital_controller.cpp, analog_controller.cpp, negcon.cpp, jogcon.cpp):
 * a digital pad, neGcon or analog stick only to the poll; the DUAL SHOCK
 * and the JogCon also to the configuration commands 0x43 to 0x4D, with
 * their own mode, configuration mode and motor mapping. They start in their
 * analog (JogCon) mode, as DuckStation's do for this kind of game;
 * selecting another kind resets them. */

#include <psyz.h>
#include <psyz/input.h>
#include <libetc.h>

#include <math.h>
#include <string.h>

#include "../internal.h"

/* Bits of the PsyZ pressed word (PadRead order) a neGcon does not have:
 * Select, L3, R3 and L2, R2, L1, Cross, Square. It reports them released. */
#define NEGCON_ABSENT_BUTTONS                                                  \
    (PADselect | PADi | PADj | PADL2 | PADR2 | PADL1 | PADRdown | PADRleft)

static PsyzAxisResponse responses[PSYZ_PAD_AXIS_COUNT] = {
    {0.0f, 1.0f, 0.0f, 1.0f, 0xFF},
    {0.0f, 1.0f, 0.0f, 1.0f, 0xFF},
    {0.0f, 1.0f, 0.0f, 1.0f, 0xFF},
    {0.0f, 1.0f, 0.0f, 1.0f, 0xFF},
};

enum {
    CTRL_IDLE = 0,  // waiting for the 0x01 address byte
    CTRL_READY = 1, // addressed, waiting for the command byte
    CTRL_COMMAND = 2,
};

typedef struct {
    PsyzControllerKind kind; // kind the state below belongs to
    // DUAL SHOCK / JogCon
    int analog;         // analog (JogCon) mode, else digital
    int config;         // configuration mode (ID 0xF3)
    int dualshock;      // DUAL SHOCK: entered configuration mode once
    int locked;         // DUAL SHOCK: mode locked by 0x44
    unsigned char status;        // second byte of every answer
    unsigned char rumble_cfg[6]; // 0x4D: motor of each actuator byte
    unsigned char motor[2];      // small, large (JogCon: command byte)
    signed char last_steering;   // JogCon dial at the poll before
    unsigned char last_motor;    // JogCon motor command (high nibble)
    // the transfer in progress
    int state, command, step, len;
    unsigned char rx[8], tx[PSYZ_PAD_BUF_LEN];
} Controller;

static Controller pads[2];

static void ResetController(int port) {
    PsyzControllerKind kind = pads[port].kind;
    memset(&pads[port], 0, sizeof(pads[port]));
    pads[port].kind = kind;
    pads[port].analog = 1;
    pads[port].status = 0x5A;
    memset(pads[port].rumble_cfg, 0xFF, sizeof(pads[port].rumble_cfg));
}

static int ValidPort(int port) { return port >= 0 && port < LEN(pads); }

int Psyz_PadsSetAxisResponse(PsyzPadAxis axis, const PsyzAxisResponse* r) {
    if ((int)axis < 0 || axis >= PSYZ_PAD_AXIS_COUNT || !r) {
        return -1;
    }
    responses[axis] = *r;
    if (!responses[axis].full) {
        responses[axis].full = 0xFF;
    }
    return 0;
}

int Psyz_PadsGetAxisResponse(PsyzPadAxis axis, PsyzAxisResponse* r) {
    if ((int)axis < 0 || axis >= PSYZ_PAD_AXIS_COUNT || !r) {
        return -1;
    }
    *r = responses[axis];
    return 0;
}

float Psyz_AxisCurve(float value, const PsyzAxisResponse* r) {
    float deadzone = r && isfinite(r->deadzone) ? r->deadzone : 0.0f;
    float saturation = r && isfinite(r->saturation) ? r->saturation : 1.0f;
    float linearity = r && isfinite(r->linearity) ? r->linearity : 0.0f;
    float scaling = r && isfinite(r->scaling) ? r->scaling : 1.0f;

    if (!isfinite(value)) {
        return 0.0f;
    }
    if (saturation <= deadzone) {
        value = value <= deadzone ? 0.0f : 1.0f;
    } else {
        value = (value - deadzone) / (saturation - deadzone);
    }
    if (value < 0.0f) {
        value = 0.0f;
    }
    if (value > 1.0f) {
        value = 1.0f;
    }
    // e^linearity: 0 is linear, positive softens the middle of the throw
    if (value != 0.0f) {
        value = powf(value, expf(linearity));
    }
    value *= scaling;
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

/* A signed host axis (-1..1) through the axis's response, keeping its sign;
 * a held digital direction is full deflection when that is further. */
static float SteeringValue(const PsyzPadInput* in) {
    float v = in->steering;
    float shaped;

    if (!isfinite(v)) {
        v = 0.0f;
    }
    shaped = Psyz_AxisCurve(v < 0.0f ? -v : v, &responses[PSYZ_PAD_AXIS_STEERING]);
    if (v < 0.0f) {
        shaped = -shaped;
    }
    if ((in->buttons & PADLleft) && !(in->buttons & PADLright)) {
        shaped = -1.0f;
    } else if ((in->buttons & PADLright) && !(in->buttons & PADLleft)) {
        shaped = 1.0f;
    }
    return shaped;
}

/* neGcon I / II / L: the trigger through its response, or full travel while
 * the digital button that stands for it is held (Cross, Square, L1). */
static unsigned char PedalByte(const PsyzPadInput* in, PsyzPadAxis axis,
                               unsigned button) {
    const PsyzAxisResponse* r = &responses[axis];
    float v = Psyz_AxisCurve(in->pedal[axis - PSYZ_PAD_AXIS_I], r);
    if (in->buttons & button) {
        v = 1.0f;
    }
    return (unsigned char)lroundf(v * (float)r->full);
}

static void PutButtons(char* frame, unsigned released) {
    frame[2] = (char)((released >> 8) & 0xFF);
    frame[3] = (char)(released & 0xFF);
}

static int HasModes(PsyzControllerKind kind) {
    return kind == PSYZ_CTRL_ANALOG_PAD || kind == PSYZ_CTRL_JOGCON;
}

/* A port without host input reports disconnected for a moment: the
 * controller (its mode, its motor mapping) is still the same one. */
static void TrackKind(int port, PsyzControllerKind kind) {
    if (kind != PSYZ_CTRL_DISCONNECTED && pads[port].kind != kind) {
        pads[port].kind = kind;
        ResetController(port);
    }
}

/* The mode button of a DUAL SHOCK / JogCon: digital or analog mode. */
void Psyz_PadsSetDigitalMode(int port, int digital) {
    if (ValidPort(port)) {
        pads[port].analog = digital == 0;
    }
}

int Psyz_PadsDigitalMode(int port) {
    return ValidPort(port) && HasModes(pads[port].kind) && !pads[port].analog;
}

int Psyz_PadsBuildFrame(int port, PsyzControllerKind kind,
                        const PsyzPadInput* in, char* frame) {
    unsigned pressed;

    if (!ValidPort(port) || !in || !frame) {
        return -1;
    }
    TrackKind(port, kind);
    memset(frame, 0, PSYZ_PAD_BUF_LEN);
    if (kind == PSYZ_CTRL_DISCONNECTED) {
        memset(frame, 0xFF, PSYZ_PAD_BUF_LEN);
        return 0;
    }
    pressed = in->buttons & 0xFFFF;
    frame[1] = (char)kind;
    switch (kind) {
    case PSYZ_CTRL_DIGITAL_PAD:
        PutButtons(frame, ~pressed);
        return 0;
    case PSYZ_CTRL_ANALOG_STICK:
        // the analog stick (SCPH-1110) has no L3/R3: they read released
        pressed &= ~(PADi | PADj);
        // fall through
    case PSYZ_CTRL_ANALOG_PAD:
        PutButtons(frame, ~pressed);
        frame[4] = (char)in->right_x;
        frame[5] = (char)in->right_y;
        frame[6] = (char)in->left_x;
        frame[7] = (char)in->left_y;
        return 0;
    case PSYZ_CTRL_NEGCON: {
        // DuckStation's steering unit: -1 is 0x00, centre 0x80, +1 0xFF
        long twist = lroundf(SteeringValue(in) * 128.0f) + 0x80;
        PutButtons(frame, ~pressed | NEGCON_ABSENT_BUTTONS);
        frame[4] = (char)(twist < 0 ? 0 : twist > 0xFF ? 0xFF : twist);
        frame[5] = (char)PedalByte(in, PSYZ_PAD_AXIS_I, PADRdown);
        frame[6] = (char)PedalByte(in, PSYZ_PAD_AXIS_II, PADRleft);
        frame[7] = (char)PedalByte(in, PSYZ_PAD_AXIS_L, PADL1);
        return 0;
    }
    case PSYZ_CTRL_JOGCON: {
        // The dial's position, from the host axis as DuckStation turns a
        // half axis into it (-128..127), sign-extended to 16 bits; the
        // controller adds the rotation and its motor command (byte 6).
        float s = SteeringValue(in);
        int half = (int)((s < 0.0f ? -s : s) * 255.0f);
        short dial = (short)(s < 0.0f ? -((half + 1) / 2) : half / 2);
        PutButtons(frame, ~pressed);
        frame[4] = (char)(dial & 0xFF);
        frame[5] = (char)((dial >> 8) & 0xFF);
        return 0;
    }
    default:
        return -1; // the platform layer builds the other kinds itself
    }
}


/* ---- the controllers' side of the serial exchange ---- */

static int FrameConnected(const char* frame) {
    return frame[0] == 0 && (unsigned char)frame[1] != PSYZ_CTRL_DISCONNECTED;
}

static unsigned char IdByte(const Controller* c) {
    switch (c->kind) {
    case PSYZ_CTRL_ANALOG_PAD:
        return c->config ? 0xF3 : c->analog ? 0x73 : 0x41;
    case PSYZ_CTRL_JOGCON:
        return c->config ? 0xF3 : c->analog ? 0xE3 : 0x41;
    default:
        return (unsigned char)c->kind;
    }
}

static int AnswerLength(unsigned char id) {
    unsigned halfwords = id & 0xF;
    return 2 + 2 * (int)(halfwords ? halfwords : 16);
}

/* DUAL SHOCK in digital mode: the left stick works the D-pad
 * (DuckStation's AnalogDPadInDigitalMode, on by default). */
static unsigned char DualShockButtonsLow(const Controller* c,
                                         const char* frame) {
    unsigned char b = (unsigned char)frame[2];
    if (!c->analog && !c->config) {
        unsigned char lx = (unsigned char)frame[6], ly = (unsigned char)frame[7];
        if (lx <= 64)
            b &= (unsigned char)~0x80; // Left
        if (lx >= 191)
            b &= (unsigned char)~0x20; // Right
        if (ly <= 64)
            b &= (unsigned char)~0x10; // Up
        if (ly >= 191)
            b &= (unsigned char)~0x40; // Down
    }
    return b;
}

static void DualShockPoll(Controller* c, const char* frame) {
    c->tx[0] = IdByte(c);
    c->tx[1] = c->status;
    c->tx[2] = DualShockButtonsLow(c, frame);
    c->tx[3] = (unsigned char)frame[3];
    for (int i = 4; i < 8; i++) {
        c->tx[i] = c->analog || c->config ? (unsigned char)frame[i] : 0;
    }
}

static void JogConMotor(Controller* c, unsigned char value) {
    c->motor[0] = value;
    c->last_motor = value >> 4;
}

static void JogConPoll(Controller* c, const char* frame) {
    signed char steering = (signed char)frame[4];
    unsigned char rotation = steering > c->last_steering   ? 1
                             : steering < c->last_steering ? 2
                                                           : 0;
    c->tx[2] = (unsigned char)frame[2];
    c->tx[3] = (unsigned char)frame[3];
    c->tx[4] = (unsigned char)steering;
    c->tx[5] = steering < 0 ? 0xFF : 0x00;
    c->tx[6] = (unsigned char)(rotation | (c->last_motor << 4));
    c->last_steering = steering;
}

static void ConfigReply(Controller* c, const unsigned char* tail) {
    c->tx[0] = IdByte(c);
    c->tx[1] = c->status;
    memcpy(&c->tx[2], tail, 6);
}

static const unsigned char kZeros[6];

/* The command byte: 0 when the controller does not take it (no ACK). */
static int StartCommand(Controller* c, unsigned char cmd, const char* frame) {
    c->command = cmd;
    switch (c->kind) {
    case PSYZ_CTRL_ANALOG_PAD:
        if (cmd == 0x42) {
            DualShockPoll(c, frame);
        } else if (cmd == 0x43) {
            if (!c->config)
                DualShockPoll(c, frame);
            else
                ConfigReply(c, kZeros);
        } else if (c->config && cmd >= 0x44 && cmd <= 0x4D &&
                   ((1u << (cmd - 0x44)) & 0x30F)) {
            static const unsigned char r47[6] = {0, 0, 2, 0, 1, 0};
            unsigned char r45[6] = {1, 2, 0, 2, 1, 0};
            r45[2] = (unsigned char)c->analog;
            ConfigReply(c, cmd == 0x45 ? r45 : cmd == 0x47 ? r47 : kZeros);
            if (cmd == 0x44) {
                memset(c->rumble_cfg, 0xFF, sizeof(c->rumble_cfg));
                c->motor[0] = c->motor[1] = 0;
            }
        } else {
            return 0;
        }
        c->len = AnswerLength(IdByte(c) == 0xF3 ? 0x73 : IdByte(c));
        return 1;
    case PSYZ_CTRL_JOGCON:
        memset(c->tx, 0, 8);
        if (cmd == 0x42 || (c->analog && cmd == 0x43)) {
            c->tx[0] = IdByte(c);
            c->tx[1] = c->status;
            JogConPoll(c, frame);
        } else if (c->config && cmd >= 0x44 && cmd <= 0x4D &&
                   ((1u << (cmd - 0x44)) & 0x30F)) {
            static const unsigned char r47[6] = {0, 0, 2, 0, 1, 0};
            unsigned char r45[6] = {1, 2, 0, 1, 1, 0};
            r45[2] = (unsigned char)c->analog;
            ConfigReply(c, cmd == 0x45 ? r45 : cmd == 0x47 ? r47 : kZeros);
            if (cmd == 0x44) {
                memset(c->rumble_cfg, 0xFF, sizeof(c->rumble_cfg));
                JogConMotor(c, 0);
            }
        } else {
            return 0;
        }
        c->len = c->analog ? 8 : 4;
        return 1;
    default:
        // digital pad, neGcon, analog stick or a frame set as it is: the
        // poll only, answered with the frame
        if (cmd != 0x42) {
            return 0;
        }
        c->tx[0] = (unsigned char)frame[1];
        c->tx[1] = 0x5A;
        memcpy(&c->tx[2], &frame[2], PSYZ_PAD_BUF_LEN - 2);
        c->len = AnswerLength(c->tx[0]);
        if (c->len > PSYZ_PAD_BUF_LEN) {
            c->len = PSYZ_PAD_BUF_LEN;
        }
        return 1;
    }
}

/* The byte the controller received at c->step of its command. */
static void CommandByte(Controller* c, unsigned char in) {
    int step = c->step;
    if (step < (int)sizeof(c->rx)) {
        c->rx[step] = in;
    }
    if (c->kind == PSYZ_CTRL_ANALOG_PAD) {
        switch (c->command) {
        case 0x42:
            if (c->dualshock) {
                if (step >= 2 && step < 7 && c->rumble_cfg[step - 2] <= 1)
                    c->motor[c->rumble_cfg[step - 2]] = in;
            } else if (step == 3) {
                // the SCPH-1150's legacy small motor
                c->motor[0] =
                    (c->rx[2] & 0xC0) == 0x40 && (c->rx[3] & 1) ? 0xFF : 0;
            }
            break;
        case 0x43:
            if (step == c->len - 1) {
                c->config = c->rx[2] == 1;
                if (c->config) {
                    c->dualshock = 1;
                    c->status = 0x5A;
                }
            }
            break;
        case 0x44:
            if (step == 2 && in <= 1)
                c->analog = in;
            else if (step == 3 && (in == 2 || in == 3))
                c->locked = in == 3;
            break;
        case 0x46:
            if (step == 2 && in <= 1) {
                static const unsigned char act[2][4] = {{1, 2, 0, 0x0A},
                                                        {1, 1, 1, 0x14}};
                memcpy(&c->tx[4], act[in], 4);
            }
            break;
        case 0x47:
            if (step == 2 && in != 0)
                memset(&c->tx[4], 0, 4);
            break;
        case 0x4C:
            if (step == 2 && in <= 1)
                c->tx[5] = in ? 0x07 : 0x04;
            break;
        case 0x4D:
            if (step >= 2 && step < 7) {
                c->tx[step] = c->rumble_cfg[step - 2];
                c->rumble_cfg[step - 2] = in;
            } else if (step == 7) {
                int small = 0, large = 0;
                for (int i = 0; i < 6; i++) {
                    small |= c->rumble_cfg[i] == 0;
                    large |= c->rumble_cfg[i] == 1;
                }
                if (!small)
                    c->motor[0] = 0;
                if (!large)
                    c->motor[1] = 0;
            }
            break;
        }
    } else if (c->kind == PSYZ_CTRL_JOGCON) {
        switch (c->command) {
        case 0x42:
            if (step >= 2 && step < 7 && c->rumble_cfg[step - 2] == 0)
                JogConMotor(c, in);
            break;
        case 0x43:
            c->config = c->rx[2] == 1 && c->analog;
            if (c->config)
                c->status = 0x5A;
            break;
        case 0x44:
            if (step == 2 && in <= 1) {
                c->analog = in;
                c->config = c->config && c->analog;
            }
            break;
        case 0x46:
            if (step == 2 && in <= 1) {
                static const unsigned char act[2][4] = {{1, 2, 0, 0x0A},
                                                        {1, 1, 1, 0x14}};
                memcpy(&c->tx[4], act[in], 4);
            }
            break;
        case 0x47:
            if (step == 2 && in != 0)
                memset(&c->tx[4], 0, 4);
            break;
        case 0x4C:
            if (step == 2 && in == 0)
                c->tx[5] = 0x04;
            else if (step == 2 && in == 1)
                c->tx[4] = 0x03;
            break;
        case 0x4D:
            if (step >= 2 && step < 7) {
                c->tx[step] = c->rumble_cfg[step - 2];
                c->rumble_cfg[step - 2] = in;
            } else if (!memchr(c->rumble_cfg, 0, sizeof(c->rumble_cfg))) {
                JogConMotor(c, 0);
            }
            break;
        }
    }
}

int Psyz_PadsTransfer(int port, const char* frame, unsigned char in,
                      unsigned char* out) {
    Controller* c;
    int ack;

    *out = 0xFF;
    if (!ValidPort(port) || !frame) {
        return 0;
    }
    c = &pads[port];
    switch (c->state) {
    case CTRL_IDLE:
        if (in != 0x01 || !FrameConnected(frame)) {
            return 0;
        }
        TrackKind(port, Psyz_PadsSetKind(port, 0, PSYZ_CTRL_QUERY_KIND));
        if ((unsigned char)frame[1] != (unsigned char)c->kind &&
            (unsigned char)frame[1] != PSYZ_CTRL_DIGITAL_PAD) {
            // a frame of another kind than the port's: answer it as it is
            TrackKind(port, (PsyzControllerKind)(unsigned char)frame[1]);
        }
        if (c->kind == PSYZ_CTRL_JOGCON) {
            memset(c->rx, 0, sizeof(c->rx));
        }
        c->state = CTRL_READY;
        return 1;
    case CTRL_READY:
        c->step = 0;
        if (!StartCommand(c, in, frame)) {
            c->state = CTRL_IDLE;
            return 0;
        }
        c->state = CTRL_COMMAND;
        break;
    default:
        break;
    }
    CommandByte(c, in);
    *out = c->tx[c->step];
    c->step++;
    ack = c->step < c->len;
    if (!ack) {
        c->state = CTRL_IDLE;
        c->step = 0;
        if (c->kind == PSYZ_CTRL_ANALOG_PAD) {
            memset(c->rx, 0, sizeof(c->rx));
        }
    }
    return ack;
}

void Psyz_PadsDeselect(int port) {
    if (ValidPort(port)) {
        pads[port].state = CTRL_IDLE;
        pads[port].step = 0;
    }
}

void Psyz_PadsGetRumble(int port, unsigned* low, unsigned* high) {
    unsigned lo = 0, hi = 0;

    if (ValidPort(port)) {
        const Controller* c = &pads[port];
        switch (c->kind) {
        case PSYZ_CTRL_ANALOG_PAD:
            // the small motor runs or not, the large one at its speed
            hi = c->motor[0] ? 0xFFFF : 0;
            lo = c->motor[1] * 0x101u;
            break;
        case PSYZ_CTRL_JOGCON: {
            // the dial motor turns (command 1 right, 2 left, 3 holding)
            // at strength 0..15
            unsigned cmd = c->motor[0] >> 4, strength = c->motor[0] & 0xF;
            if (cmd >= 1 && cmd <= 3) {
                lo = hi = strength * 0x1111u;
            }
            break;
        }
        default:
            break;
        }
    }
    if (low) {
        *low = lo;
    }
    if (high) {
        *high = hi;
    }
}
