#include <psyz.h>
#include <libetc.h>

#include "libapi.h"
#include "../internal.h"
#include "../timing_profile.h"

#include <psyz/log.h>

/* BIOS A(2Fh)/A(30h): unsigned 32-bit state, 15-bit result.
 * https://psx-spx.consoledev.net/ps1/kernelbios/misc-functions/
 * Before any srand the state is the word the kernel leaves at 0x80009010:
 * 0x24040001 with the SCPH-1001 BIOS (as on DuckStation), so a game that
 * never seeds draws the same numbers as there. */
static uint32_t biosRandomState = UINT32_C(0x24040001);
/* The ROM routines with the A0 stub take BIOS_RAND_CYCLES and
 * BIOS_SRAND_CYCLES (timing_profile.h). */
int Psyz_Rand(void) {
    biosRandomState = biosRandomState * UINT32_C(0x41c64e6d) + UINT32_C(0x3039);
    Psyz_SpendCycles(BIOS_RAND_CYCLES);
    return (int)((biosRandomState >> 16) & 0x7fff);
}
void Psyz_Srand(unsigned int seed) {
    biosRandomState = (uint32_t)seed;
    Psyz_SpendCycles(BIOS_SRAND_CYCLES);
}
unsigned int Psyz_RandState(void) { return biosRandomState; }

void MyPadInit(int mode);
void PadInit(int mode) { MyPadInit(mode); }

static unsigned UnpackPadBits(const char* buf) {
    if ((unsigned char)buf[0] != 0) {
        return 0; // port disconnected or frame not valid
    }
    switch ((unsigned char)buf[1]) {
    case PSYZ_CTRL_DIGITAL_PAD:
    case PSYZ_CTRL_ANALOG_STICK:
    case PSYZ_CTRL_ANALOG_PAD:
        break;
    default:
        return 0; // not a pad-like device, switches are not in bytes 2-3
    }
    unsigned int buttons0 = (unsigned char)buf[2];
    unsigned int buttons1 = (unsigned char)buf[3];
    unsigned int padq = (buttons0 << 8) | buttons1;
    return (~padq) & 0xFFFF;
}

unsigned int PadRead(int id) {
    char p0[PSYZ_PAD_BUF_LEN], p1[PSYZ_PAD_BUF_LEN];
    Psyz_PadsGet(0, p0, sizeof(p0));
    Psyz_PadsGet(1, p1, sizeof(p1));
    unsigned r = UnpackPadBits(p0) | (UnpackPadBits(p1) << 16);

    // PS1 gamepad cannot have opposite D-pad pressed at the same time
    // solves a bug on Castlevania SOTN when pressing both Left and Right
    // where its behavior is undefined.
    if (r & PADLleft && r & PADLright) {
        r &= ~(PADLleft | PADLright);
    }
    if (r & PADLup && r & PADLdown) {
        r &= ~(PADLup | PADLdown);
    }

    return r;
}

void PadStop(void) { NOT_IMPLEMENTED; }

static long video_mode = 0;
long GetVideoMode() { return video_mode; }
long SetVideoMode(long mode) {
    long prev = video_mode;
    video_mode = mode;
    Psyz_GpuConsoleVideoMode(mode == MODE_PAL);
    return prev;
}

static void (*dma_callbacks[7])(void);
static unsigned dma_pending;
static int dma_dispatching;

void* DMACallback(int dma, void (*func)()) {
    if (dma < 0 || dma >= 7)
        return NULL;
    void (*previous)(void) = dma_callbacks[dma];
    dma_callbacks[dma] = func;
    return (void*)previous;
}

/* Transfers finish synchronously on PC. A callback may start another one;
 * dispatch its completion after the current callback returns, as DMA would. */
void Psyz_DmaComplete(unsigned channel) {
    if (channel >= 7)
        return;
    dma_pending |= 1u << channel;
    if (dma_dispatching)
        return;
    dma_dispatching = 1;
    while (dma_pending) {
        unsigned next = 0;
        while (!(dma_pending & (1u << next)))
            ++next;
        dma_pending &= ~(1u << next);
        void (*callback)(void) = dma_callbacks[next];
        if (callback)
            callback();
    }
    dma_dispatching = 0;
}

int InitTAP(char* bufA, long lenA, char* bufB, long lenB) {
    LOG_ONCE("forwarding to InitPAD");
    return InitPAD(bufA, lenA, bufB, lenB);
}

void StartTAP(void) {
    LOG_ONCE("forwarding to StartPAD");
    StartPAD();
}

/* PSY-Q 4.2 direct-connection pad library (libpad). The receive buffers are
 * the InitPAD ones, refreshed every VSync with the PS1 frame for the kind
 * Psyz_PadsSetKind selects. The tables libpad reads from a controller (its
 * modes, actuators) are those DuckStation's controllers answer with:
 *   DUAL SHOCK (PSYZ_CTRL_ANALOG_PAD): modes {digital 0x4, analog 0x7}, two
 *     motors; the ID of the current mode is also its extended ID
 *   JogCon (PSYZ_CTRL_JOGCON): modes {digital 0x4, JogCon 0x300}, one motor
 * Both report the ready (stable) state. Every other pad-like kind (digital
 * pad, neGcon) is a controller without the extended protocol. */
enum {
    PAD_STATE_DISCON = 0,
    PAD_STATE_FIND_CTP1 = 2,
    PAD_STATE_STABLE = 6,
};

typedef struct {
    unsigned short modes[2];          // InfoModeIdTable: digital, analog
    unsigned char acts[2][5];         // PadInfoAct terms 1..5
    int act_count;
} PadExtendedInfo;

static const PadExtendedInfo pad_dualshock = {
    {0x4, 0x7}, {{1, 2, 0, 10, 0}, {1, 1, 1, 20, 0}}, 2};
static const PadExtendedInfo pad_jogcon = {
    {0x4, 0x300}, {{1, 2, 0, 10, 0}}, 1};

static int PadDirectPort(int port) {
    if (port & ~0x10) {
        return -1; // Multi Tap channels B-D are never connected
    }
    return port >> 4;
}

/* The extended tables of a stable controller in port selector p, or NULL. */
static const PadExtendedInfo* PadDirectInfo(int port, int* p_out, char* frame) {
    int p = PadDirectPort(port);
    if (p < 0) {
        return NULL;
    }
    Psyz_PadsGet(p, frame, PSYZ_PAD_BUF_LEN);
    *p_out = p;
    if (frame[0] != 0 || (unsigned char)frame[1] == PSYZ_CTRL_DISCONNECTED) {
        return NULL;
    }
    switch (Psyz_PadsSetKind(p, 0, PSYZ_CTRL_QUERY_KIND)) {
    case PSYZ_CTRL_ANALOG_PAD:
        return &pad_dualshock;
    case PSYZ_CTRL_JOGCON:
        return &pad_jogcon;
    default:
        return NULL;
    }
}

static int PadDirectState(int port) {
    char frame[PSYZ_PAD_BUF_LEN];
    int p = -1;
    if (PadDirectInfo(port, &p, frame)) {
        return PAD_STATE_STABLE;
    }
    if (p < 0 || frame[0] != 0 ||
        (unsigned char)frame[1] == PSYZ_CTRL_DISCONNECTED) {
        return PAD_STATE_DISCON;
    }
    return PAD_STATE_FIND_CTP1;
}

/* PSY-Q libpad clears its driver state (a 0x1E0-byte BIOS bzero) and sets
 * up the exchange (PAD_INIT_DIRECT_CYCLES, PAD_START_COM_CYCLES). */

void PadInitDirect(u_char* pad1, u_char* pad2) {
    Psyz_SpendCycles(PAD_INIT_DIRECT_CYCLES);
    for (int p = 0; p < 2; p++) {
        Psyz_PadsSetActuators(p, NULL, 0);
        Psyz_PadsSetActuatorAlign(p, NULL);
    }
    InitPAD((char*)pad1, PSYZ_PAD_BUF_LEN, (char*)pad2, PSYZ_PAD_BUF_LEN);
}

static int pad_driver_running;
int Psyz_PadDriverRunning(void) { return pad_driver_running; }

void PadStartCom(void) {
    // receive buffers are already refreshed on every VSync
    Psyz_SpendCycles(PAD_START_COM_CYCLES);
    pad_driver_running = 1;
}

int PadGetState(int port) { return PadDirectState(port); }

int PadInfoMode(int port, int term, int offs) {
    char frame[PSYZ_PAD_BUF_LEN];
    int p = -1;
    const PadExtendedInfo* info = PadDirectInfo(port, &p, frame);
    int cur_id = ((unsigned char)frame[1] >> 4) & 0xF;
    if (p < 0 || frame[0] != 0 ||
        (unsigned char)frame[1] == PSYZ_CTRL_DISCONNECTED) {
        return 0;
    }
    if (!info) {
        return term == 1 ? cur_id : 0; // InfoModeCurID only
    }
    int offset = !Psyz_PadsDigitalMode(p);
    switch (term) {
    case 1: // InfoModeCurID
        return cur_id;
    case 2: // InfoModeCurExID: the current entry of the mode table
        return info->modes[offset];
    case 3: // InfoModeCurExOffs
        return offset;
    case 4: // InfoModeIdTable
        if (offs == -1)
            return LEN(info->modes);
        if (offs >= 0 && offs < LEN(info->modes))
            return info->modes[offs];
        return 0;
    default:
        return 0;
    }
}

int PadInfoAct(int port, int actno, int term) {
    char frame[PSYZ_PAD_BUF_LEN];
    int p = -1;
    const PadExtendedInfo* info = PadDirectInfo(port, &p, frame);
    if (!info) {
        return 0;
    }
    if (actno < 0) {
        return info->act_count;
    }
    if (actno >= info->act_count || term < 1 || term > 5) {
        return 0;
    }
    return info->acts[actno][term - 1];
}

int PadInfoComb(int port, int listno, int offs) {
    (void)port;
    (void)listno;
    (void)offs;
    return 0; // the motors have no combination restrictions
}

void PadSetAct(int port, u_char* data, int len) {
    // sent at every exchange from now on, through the PadSetActAlign table
    int p = PadDirectPort(port);
    if (p >= 0) {
        Psyz_PadsSetActuators(p, data, len);
    }
}

int PadSetActAlign(int port, char* data) {
    char frame[PSYZ_PAD_BUF_LEN];
    int p = -1;
    if (!PadDirectInfo(port, &p, frame)) {
        return 0;
    }
    Psyz_PadsSetActuatorAlign(p, (const unsigned char*)data);
    return 1;
}

int PadSetMainMode(int port, int offs, int lock) {
    char frame[PSYZ_PAD_BUF_LEN];
    int p = -1;
    (void)lock; // the host has no mode selector button to lock
    if (!PadDirectInfo(port, &p, frame) || offs < 0 || offs > 1) {
        return 0;
    }
    Psyz_PadsSetDigitalMode(p, offs == 0);
    return 1;
}

void PadInitMtap(u_char* pad1, u_char* pad2) {
    // from PSY-Q 4.2
    NOT_IMPLEMENTED;
}
