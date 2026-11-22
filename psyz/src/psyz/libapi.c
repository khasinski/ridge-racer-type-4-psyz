#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <psyz.h>
#include <psyz/log.h>
#include <kernel.h>
#include <libetc.h>
#include "libgpu.h"
#include "../draw.h"
#include "../internal.h"

#undef _get_errno // Windows: avoid conflicts
#undef undelete   // macOS: avoid conflicts

typedef struct {
    char frame[PSYZ_PAD_BUF_LEN];   // last frame published by the platform
    PsyzControllerKind channels[4]; // only channel 0 until multitap lands
} ControllerPort;

static ControllerPort ports[2] = {
    {{0},
     {
         PSYZ_CTRL_DIGITAL_PAD,
         PSYZ_CTRL_DISCONNECTED,
         PSYZ_CTRL_DISCONNECTED,
         PSYZ_CTRL_DISCONNECTED,
     }},
    {{0},
     {
         PSYZ_CTRL_DIGITAL_PAD,
         PSYZ_CTRL_DISCONNECTED,
         PSYZ_CTRL_DISCONNECTED,
         PSYZ_CTRL_DISCONNECTED,
     }},
};

void Psyz_PadsPoll(void); // implemented by the platform layer
static int pads_sampled = 0; // avoid more than one input polling per frame
static char* pad_buffers[2];
static int pad_buffer_lens[2];
static PsyzPadsInputCb pads_input_cb;

PsyzPadsInputCb Psyz_SetPadsInputCb(PsyzPadsInputCb cb) {
    PsyzPadsInputCb prev = pads_input_cb;
    pads_input_cb = cb;
    return prev;
}

const char* Psyz_PadsFrame(int port) {
    return port >= 0 && port < LEN(ports) ? ports[port].frame : NULL;
}

/* The controllers answer with the host input sampled at the blank (or the
 * frames an input callback sets). libpad's handler (libpad.c) exchanges
 * with them at each blank and keeps its receive buffers itself; without
 * libpad the InitPAD buffers take the frames at once. */
static void ReadPadsOnVsync(void) {
    if (!pad_buffers[0] && !pad_buffers[1] && !Psyz_PadDriverRunning()) {
        pads_sampled = 0; // nothing reads them: poll when asked
        return;
    }
    pads_sampled = 1;
    Psyz_PadsPoll();
    if (pads_input_cb) {
        pads_input_cb();
    }
    if (Psyz_PadDriverRunning()) {
        Psyz_LibpadVBlank();
        return;
    }
    for (int p = 0; p < LEN(ports); p++) {
        if (pad_buffers[p]) {
            Psyz_PadsGet(p, pad_buffers[p], pad_buffer_lens[p]);
        }
    }
}

static PsyzVSyncCb g_PsyzVsyncCb = NULL;
PsyzVSyncCb Psyz_SetVSyncCb(PsyzVSyncCb cb) {
    PsyzVSyncCb prev = g_PsyzVsyncCb;
    g_PsyzVsyncCb = cb;
    return prev;
}

/* The kernel's time when the latest blocking VSync returned, from which
 * VSync(1) counts lines (the PS1's line counter latch). */
static uint64_t vsync_return_ns;

/* The work of a blank: the controllers, the host's hook, the vblank
 * interrupt (the game's VSync callbacks and events). */
void Psyz_VSyncBlank(void) {
    Psyz_CardBlankBegin();
    ReadPadsOnVsync();
    if (g_PsyzVsyncCb)
        g_PsyzVsyncCb();
    Psyz_KernelVBlank();
}

/* SDK VSync(n) waits for the interval since the previous blocking call,
 * then at least the next blank. Queries do not reset that interval. */
static unsigned int last_vsync_count;

static unsigned LineNs(void) {
    return Psyz_GpuCrtcFieldLines() == 314 ? 64017 : 63563;
}

/* On a real-time clock the blanks come from the wall clock, also while the
 * application works or polls VSync(1) past one: each is serviced (pads,
 * callbacks, the vblank interrupt) the next time VSync looks at the count,
 * as the console's interrupt would have. The offline and unpaced clocks
 * service their blanks where they advance them. */
static unsigned int wall_serviced_count;
static int wall_serviced_valid;

static int WallBlanks(void) {
    return !Psyz_KernelOfflineClock() &&
           Psyz_VideoGetVsyncMode() != PSYZ_VSYNC_LIMITLESS;
}

static unsigned int ServiceWallBlanks(void) {
    unsigned int now = (unsigned int)Psyz_VideoVSync(-1);
    unsigned int missed = now - wall_serviced_count;
    if (!wall_serviced_valid || missed > 600) {
        missed = 0; // the first look, or a resynchronised clock
    }
    wall_serviced_count = now;
    wall_serviced_valid = 1;
    while (missed--) {
        Psyz_VSyncBlank();
    }
    return now;
}

static unsigned int WaitBlank(unsigned int now) {
    unsigned int before = now;
    Psyz_VideoVSync(0);
    if (WallBlanks()) {
        return ServiceWallBlanks();
    }
    wall_serviced_valid = 0;
    now = (unsigned int)Psyz_VideoVSync(-1);
    unsigned int advanced = now - before;
    while (advanced--) {
        Psyz_VSyncBlank();
    }
    return now;
}

int VSync(int mode) {
    Psyz_KernelPoll();
    if (mode < 0)
        return Psyz_VideoVSync(-1);
    // a wait (not a query, which interrupt callbacks also make)
    if (WallBlanks()) {
        ServiceWallBlanks();
    }
    if (mode == 1) {
        /* The lines since the latest VSync(0) returned. A game polls this
         * in a loop (until a frame has taken so many lines): on the offline
         * clock, where its work takes no time, each look moves the clock a
         * line on, so such a loop crosses the blanks a frame of that length
         * would. */
        if (Psyz_KernelOfflineClock()) {
            Psyz_KernelWaitUntilNs(Psyz_KernelNowNs() + LineNs());
            return (unsigned short)((Psyz_KernelNowNs() - vsync_return_ns) / LineNs());
        }
        return (unsigned short)Psyz_VideoVSync(1);
    }

    int elapsed = (unsigned short)Psyz_VideoVSync(1);
    unsigned int now = (unsigned int)Psyz_VideoVSync(-1);
    unsigned int target =
        mode > 1 ? last_vsync_count + (unsigned int)mode : now + 1;
    if ((int32_t)(target - now) < 1)
        target = now + 1;
    do {
        now = WaitBlank(now);
    } while ((int32_t)(now - target) < 0);
    Psyz_CdServiceDrive();
    vsync_return_ns = Psyz_KernelNowNs();
    /* Keep periodic waits on their scheduled blank. Native presentation can
     * return just after it; using that late return as the next origin adds a
     * blank repeatedly and lets FMV pictures drift behind device audio.
     * A whole missed interval resynchronizes rather than replaying a stall. */
    last_vsync_count =
        mode > 1 && now - target < (unsigned int)mode ? target : now;
    return elapsed;
}

PsyzControllerKind Psyz_PadsSetKind(
    int port, int channel, PsyzControllerKind kind) {
    if (port < 0 || port >= LEN(ports)) {
        return PSYZ_CTRL_ERROR;
    }
    if (channel < 0 || channel >= LEN(ports->channels)) {
        return PSYZ_CTRL_ERROR;
    }
    if (kind == PSYZ_CTRL_QUERY_KIND) {
        return ports[port].channels[channel];
    }
    PsyzControllerKind prev = ports[port].channels[channel];
    ports[port].channels[channel] = kind;
    return prev;
}

void Psyz_PadsSet(int port, const char* src, int len) {
    if (port < 0 || port >= LEN(ports) || !src || len <= 0) {
        return;
    }
    if (len > PSYZ_PAD_BUF_LEN) {
        len = PSYZ_PAD_BUF_LEN;
    }
    memcpy(ports[port].frame, src, len);
    pads_sampled = 1;
}

void Psyz_PadsGet(int port, char* dst, int len) {
    if (port < 0 || port >= LEN(ports) || !dst || len <= 0) {
        return;
    }
    if (!pads_sampled) {
        pads_sampled = 1;
        Psyz_PadsPoll();
    }
    if (len > PSYZ_PAD_BUF_LEN) {
        len = PSYZ_PAD_BUF_LEN;
    }
    memcpy(dst, ports[port].frame, len);
}

int InitPAD(char* bufA, int lenA, char* bufB, int lenB) {
    PadInit(0);
    if (bufA) {
        memset(bufA, 0, lenA);
    } else {
        WARNF("bufA is NULL");
    }
    if (bufB) {
        memset(bufB, 0, lenB);
    } else {
        WARNF("bufB is NULL");
    }
    pad_buffers[0] = bufA;
    pad_buffer_lens[0] = lenA;
    pad_buffers[1] = bufB;
    pad_buffer_lens[1] = lenB;
    return 1;
}

long StartPAD(void) {
    NOT_IMPLEMENTED;
    return 1;
}

void StopPAD(void) { NOT_IMPLEMENTED; }

int PAD_init(int type, void* unused) {
    PadInit(0);
    return 1;
}

int PAD_dr(int port, char* dst) {
    if (port < 0 || port > 1 || !dst) {
        return 0;
    }
    Psyz_PadsGet(port, dst, PSYZ_PAD_BUF_LEN);
    return PSYZ_PAD_BUF_LEN;
}

void _96_remove(void) { NOT_IMPLEMENTED; }

long ReadInitPadFlag(void) {
    NOT_IMPLEMENTED;
    return 0;
}

void ChangeClearPAD(long val) { (void)val; }

void SystemError(char c, long n) {
    NOT_IMPLEMENTED;
    ERRORF("SystemError('%c', 0x%X)", c, n);
}

struct DIRENTRY* my_firstfile(char* dirPath, struct DIRENTRY* firstEntry);
struct DIRENTRY* firstfile(char* dirPath, struct DIRENTRY* firstEntry) {
    // A card taken out (its host directory remains) has no entry.
    if (dirPath && dirPath[0] == 'b' && dirPath[1] == 'u' &&
        dirPath[2] >= '0' && dirPath[2] <= '1' && dirPath[3] >= '0' &&
        dirPath[3] <= '3' &&
        (dirPath[3] != '0' || Psyz_CardSlotState(dirPath[2] - '0') == PSYZ_CARD_NONE)) {
        return NULL;
    }
    return my_firstfile(dirPath, firstEntry);
}

struct DIRENTRY* my_nextfile(struct DIRENTRY* outEntry);
struct DIRENTRY* nextfile(struct DIRENTRY* outEntry) { return my_nextfile(outEntry); }

long my_erase(char* path);
long erase(char* path) { return my_erase(path); }

long psyz_undelete(char* name) {
    NOT_IMPLEMENTED;
    return 0;
}

long my_format(char* fs);
long format(char* fs) { return my_format(fs); }

int psyz_get_errno(void) {
    NOT_IMPLEMENTED;
    return 0;
}

/* BIOS A(28h)/A(2Ah)/A(2Bh) (system.h). */

void Psyz_BiosBzero(void* dst, int len) {
    if (dst && len > 0) {
        memset(dst, 0, (size_t)len);
    }
}

void* Psyz_BiosMemset(void* dst, int c, int len) {
    if (dst && len > 0) {
        memset(dst, c & 0xFF, (size_t)len);
    }
    return dst;
}

void* Psyz_BiosMemcpy(void* dst, const void* src, int len) {
    if (dst && src && len > 0) {
        // the ROM copies forwards byte by byte, overlapping or not
        unsigned char* d = dst;
        const unsigned char* s = src;
        for (int i = 0; i < len; i++) {
            d[i] = s[i];
        }
    }
    return dst;
}
