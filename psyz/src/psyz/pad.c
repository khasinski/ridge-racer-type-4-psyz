/* Controller frames of the emulated PS1 controllers.
 *
 * The platform layer samples the host (buttons, sticks, triggers) into a
 * PsyzPadInput; Psyz_PadsBuildFrame turns it into the receive buffer the
 * controller kind sends, byte for byte as on the console:
 *   https://psx-spx.consoledev.net/controllersandmemorycards/
 *
 * Analog inputs that the host only approximates (a stick for the neGcon
 * twist or the JogCon dial, triggers for the neGcon's analog buttons) go
 * through a response curve with DuckStation's parameters (dead zone,
 * saturation, linearity, scaling), so values tuned there carry over.
 *
 * The DUAL SHOCK and the JogCon have two modes. They start in their analog
 * (JogCon) mode, as DuckStation's do; libpad's PadSetMainMode switches the
 * mode (Psyz_PadsSetDigitalMode). Selecting another kind resets it.
 *
 * libpad also sends the actuator bytes at every exchange (PadSetAct,
 * PadSetActAlign): Psyz_PadsExchange reads them so the platform layer can
 * drive host rumble and the JogCon reports its last motor command. */

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

static struct {
    PsyzControllerKind kind; // kind the state below belongs to
    int digital_mode;        // DUAL SHOCK / JogCon switched to digital mode
    const unsigned char* act_data; // PadSetAct buffer
    int act_len;
    unsigned char act_align[6]; // PadSetActAlign table (0xFF: not sent)
    int act_aligned;
    unsigned char sent[6];  // actuator bytes of the latest exchange
    short dial;             // JogCon dial of the latest published frame
    unsigned char jog_cmd;  // JogCon motor command of the exchange before
} pads[2];

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

void Psyz_PadsSetDigitalMode(int port, int digital) {
    if (ValidPort(port)) {
        pads[port].digital_mode = digital != 0;
    }
}

int Psyz_PadsDigitalMode(int port) {
    return ValidPort(port) && pads[port].digital_mode;
}

/* A port without host input reports disconnected for a moment: the
 * controller (its mode, the JogCon's dial) is still the same one. */
static void TrackKind(int port, PsyzControllerKind kind) {
    if (kind != PSYZ_CTRL_DISCONNECTED && pads[port].kind != kind) {
        pads[port].kind = kind;
        pads[port].digital_mode = 0;
        pads[port].dial = 0;
        pads[port].jog_cmd = 0;
        memset(pads[port].sent, 0, sizeof(pads[port].sent));
    }
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
    if ((kind == PSYZ_CTRL_ANALOG_PAD || kind == PSYZ_CTRL_JOGCON) &&
        pads[port].digital_mode) {
        kind = PSYZ_CTRL_DIGITAL_PAD;
    }
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
        // The dial's 16-bit position, from the host axis as DuckStation
        // turns a half axis into it (-128..127); byte 6 holds the rotation
        // since the frame before (1 forward, 2 back) and the motor command
        // of the exchange before in its high nibble.
        float s = SteeringValue(in);
        int half = (int)((s < 0.0f ? -s : s) * 255.0f);
        short dial = (short)(s < 0.0f ? -((half + 1) / 2) : half / 2);
        unsigned char rotation = dial > pads[port].dial   ? 1
                                 : dial < pads[port].dial ? 2
                                                          : 0;
        PutButtons(frame, ~pressed);
        frame[4] = (char)(dial & 0xFF);
        frame[5] = (char)((dial >> 8) & 0xFF);
        frame[6] = (char)(rotation | (pads[port].jog_cmd << 4));
        return 0;
    }
    default:
        return -1; // the platform layer builds the other kinds itself
    }
}

void Psyz_PadsSetActuators(int port, const unsigned char* data, int len) {
    if (ValidPort(port)) {
        pads[port].act_data = data;
        pads[port].act_len = data && len > 0 ? len : 0;
    }
}

void Psyz_PadsSetActuatorAlign(int port, const unsigned char* align) {
    if (!ValidPort(port)) {
        return;
    }
    if (!align) {
        pads[port].act_aligned = 0;
        return;
    }
    memcpy(pads[port].act_align, align, sizeof(pads[port].act_align));
    pads[port].act_aligned = 1;
}

void Psyz_PadsExchange(int port, const char* published) {
    unsigned char sent[6];

    if (!ValidPort(port)) {
        return;
    }
    memset(sent, 0, sizeof(sent));
    if (pads[port].act_aligned && pads[port].act_data) {
        for (int i = 0; i < LEN(sent); i++) {
            unsigned a = pads[port].act_align[i];
            if (a < (unsigned)pads[port].act_len) {
                sent[i] = pads[port].act_data[a];
            }
        }
    }
    if (published && (unsigned char)published[0] == 0 &&
        (unsigned char)published[1] == PSYZ_CTRL_JOGCON) {
        pads[port].dial =
            (short)((unsigned char)published[4] |
                    ((unsigned)(unsigned char)published[5] << 8));
    }
    // the frame after this exchange reports the command of the one before
    pads[port].jog_cmd = pads[port].sent[0] >> 4;
    memcpy(pads[port].sent, sent, sizeof(sent));
}

void Psyz_PadsGetRumble(int port, unsigned* low, unsigned* high) {
    unsigned lo = 0, hi = 0;

    if (ValidPort(port) && pads[port].act_aligned) {
        const unsigned char* sent = pads[port].sent;
        switch (pads[port].kind) {
        case PSYZ_CTRL_ANALOG_PAD:
            // DUAL SHOCK: byte 0 the small motor (bit 0 on), byte 1 the
            // large motor's speed
            hi = (sent[0] & 1) ? 0xFFFF : 0;
            lo = sent[1] * 0x101u;
            break;
        case PSYZ_CTRL_JOGCON: {
            // JogCon: the dial motor turns (command 1 right, 2 left, 3
            // holding) at strength 0..15
            unsigned cmd = sent[0] >> 4, strength = sent[0] & 0xF;
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
