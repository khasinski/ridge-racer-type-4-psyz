#ifndef PSYZ_INPUT_H
#define PSYZ_INPUT_H

/**
 * @file input.h
 * @brief Controller and gamepad input endpoints.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Controller identifiers to emulate multiple PS1 controller kinds
 *
 * https://problemkaputt.de/psxspx-controllers-communication-sequence.htm
 */
/* Receive buffers (byte 0 status 0x00, byte 1 the kind): bytes 2-3 are the
 * buttons, active low (byte 2: Select L3 R3 Start Up Right Down Left from
 * bit 0, byte 3: L2 R2 L1 R1 Triangle Circle Cross Square), then
 *   digital pad   nothing more
 *   analog pad    right X, right Y, left X, left Y (0x00 left/up, 0x80 centre)
 *   neGcon        twist (0x00 left, 0x80 centre, 0xFF right), I, II, L
 *                 (0x00 released .. 0xFF); its buttons are Start, the D-pad,
 *                 A (Circle's bit), B (Triangle's) and R (R1's), the other
 *                 bits read released
 *   JogCon        dial position (16-bit, little endian), then the rotation
 *                 since the last frame (1 forward, 2 back) with the motor
 *                 command of the exchange before in the high nibble
 * A DUAL SHOCK or JogCon in digital mode sends the digital pad's frame. */
typedef enum {
    PSYZ_CTRL_ERROR = 0x00,         /**< Failed to set controller param */
    PSYZ_CTRL_QUERY_KIND = 0x00,    /**< Query current kind without changing it */
    PSYZ_CTRL_MOUSE = 0x12,         /**< TODO document buffer format */
    PSYZ_CTRL_NEGCON = 0x23,        /**< Namco neGcon */
    PSYZ_CTRL_DIGITAL_PAD = 0x41,   /**< Digital pad (SCPH-1080) */
    PSYZ_CTRL_ANALOG_STICK = 0x53,  /**< Analog stick (SCPH-1110) */
    PSYZ_CTRL_ANALOG_PAD = 0x73,    /**< DUAL SHOCK in analog mode */
    PSYZ_CTRL_KEYBOARD = 0x96,      /**< TODO document buffer format */
    PSYZ_CTRL_JOGCON = 0xE3,        /**< Namco JogCon in JogCon mode */
    PSYZ_CTRL_DISCONNECTED = 0xFF,  /**< No controller: every byte 0xFF */
} PsyzControllerKind;

/**
 * @brief Select which controller kind a port emulates
 *
 * Sets the controller kind reported on @p port and @p channel.
 * Defaults to PSYZ_CTRL_DIGITAL_PAD for better compatibility with early games.
 * When the reserved PSYZ_CTRL_QUERY_KIND value is passed, fetch the current
 * controller kind for the specified @p port and @p channel as returned value.
 *
 * @param port Controller port (0 or 1)
 * @param channel Multitap channel (0-3). Currently reserved and unused.
 * @param kind Controller kind to emulate, or PSYZ_CTRL_QUERY_KIND
 * @return Previously selected kind, or PSYZ_CTRL_ERROR on invalid arguments
 */
PsyzControllerKind Psyz_PadsSetKind(
    int port, int channel, PsyzControllerKind kind);

/**
 * Size of the controller receive buffer the BIOS fills on real hardware:
 * byte 0 = status, byte 1 = controller kind, then the per-kind payload.
 * https://problemkaputt.de/psxspx-controllers-and-memory-cards.htm
 */
#define PSYZ_PAD_BUF_LEN 34

/**
 * @brief Retrieve the internal 34-byte controller frame for a port
 *
 * Copies the latest hardware-faithful SIO frame for @p port into @p dst.
 * Direct low-level entrypoint for libetc/libapi. Depending on the backend
 * implementation, triggers an OS poll of gamepad or mapped keyboard.
 *
 * @param port Controller port (0 or 1)
 * @param dst Buffer to write into
 * @param len Buffer length in bytes (at most PSYZ_PAD_BUF_LEN is copied)
 */
void Psyz_PadsGet(int port, char* dst, int len);

/**
 * @brief Set the internal 34-byte controller frame.
 *
 * Called by the platform input backend to store the same frame retrieved with
 * Psyz_PadsGet. It can be used to emulate input as part of automated tests.
 *
 * @param port Controller port (0 or 1)
 * @param src Buffer to write from
 * @param len Buffer length in bytes
 */
void Psyz_PadsSet(int port, const char* src, int len);

/** Analog inputs that the host approximates with a stick or a trigger. */
typedef enum {
    PSYZ_PAD_AXIS_STEERING = 0, /**< neGcon twist, JogCon dial */
    PSYZ_PAD_AXIS_I,            /**< neGcon I */
    PSYZ_PAD_AXIS_II,           /**< neGcon II */
    PSYZ_PAD_AXIS_L,            /**< neGcon L */
    PSYZ_PAD_AXIS_COUNT
} PsyzPadAxis;

/**
 * Response of an approximated analog input, as DuckStation's neGcon axis
 * modifiers: the travelled fraction past @c deadzone up to @c saturation,
 * raised to e^@c linearity (0 linear), times @c scaling, clamped to 0..1.
 * A pedal (I, II, L) reports @c full at full travel (0 means 0xFF).
 * The defaults (0, 1, 0, 1, 0xFF) leave the host value unchanged.
 */
typedef struct {
    float deadzone, saturation, linearity, scaling;
    unsigned char full;
} PsyzAxisResponse;

int Psyz_PadsSetAxisResponse(PsyzPadAxis axis, const PsyzAxisResponse* r);
int Psyz_PadsGetAxisResponse(PsyzPadAxis axis, PsyzAxisResponse* r);
/** The response curve for a magnitude 0..1. */
float Psyz_AxisCurve(float value, const PsyzAxisResponse* r);

/** What the host holds for one controller port. */
typedef struct {
    unsigned buttons; /**< pressed, PadRead's bits (PADL2 .. PADLleft) */
    /** DUAL SHOCK sticks as sent (0x00 left/up, 0x80 centre, 0xFF) */
    unsigned char right_x, right_y, left_x, left_y;
    float steering; /**< twist / dial source, -1 left .. 1 right */
    float pedal[3]; /**< I, II, L sources, 0 released .. 1 */
} PsyzPadInput;

/**
 * @brief Build the receive buffer a controller kind sends for @p input
 *
 * Writes PSYZ_PAD_BUF_LEN bytes to @p frame. The neGcon and JogCon take
 * steering and pedals through their PsyzAxisResponse; a held Left/Right
 * steers fully, Cross / Square / L1 press I / II / L fully. Returns -1 for
 * kinds it does not build (mouse, keyboard).
 */
int Psyz_PadsBuildFrame(int port, PsyzControllerKind kind,
                        const PsyzPadInput* input, char* frame);

/** DUAL SHOCK / JogCon mode (libpad PadSetMainMode); reset by a new kind. */
void Psyz_PadsSetDigitalMode(int port, int digital);
int Psyz_PadsDigitalMode(int port);

/**
 * @brief Host rumble for the actuator bytes libpad sent at the last exchange
 *
 * DUAL SHOCK: the large motor drives @p low, the small one @p high; JogCon:
 * the dial motor's strength drives both. Values are 0..0xFFFF.
 */
void Psyz_PadsGetRumble(int port, unsigned* low, unsigned* high);

#ifdef __cplusplus
}
#endif

#endif
