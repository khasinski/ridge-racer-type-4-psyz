#include <stdio.h>
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
    Psyz_KernelLockInterrupts();
    void (*previous)(void) = dma_callbacks[dma];
    dma_callbacks[dma] = func;
    Psyz_KernelUnlockInterrupts();
    return (void*)previous;
}

/* Transfers finish synchronously on PC. A callback may start another one;
 * dispatch its completion after the current callback returns, as DMA would.
 * The MDEC output's channel is the one whose interrupt the game enables
 * (DICR on DuckStation: it and the SPU's): its callback runs in that
 * interrupt's exception (Psyz_DmaIrqComplete); the others' at once. */
static int dma_irq_host = -1; // Psyz_DmaIrqComplete: its exception's host

static void DispatchDma(unsigned channel, void (*callback)(void)) {
    if (channel != 1 || dma_irq_host < 0) {
        callback();
        return;
    }
    unsigned chain = Psyz_CardChainInstalled() ? BIOS_CARD_CHAIN_CYCLES : 0;
    unsigned total = KERNEL_IRQ_CYCLES + DMA_IRQ_DISPATCH_CYCLES + chain;
    unsigned before = DEVICE_IRQ_TO_HANDLER_CYCLES;
    unsigned after = total - before;
    if (dma_irq_host) { // raised during another exception: libetc's reread
        before = dma_irq_host == 2 ? DMA_AFTER_DEVICE_TO_HANDLER_CYCLES
                                   : DMA_AFTER_LINE_TO_HANDLER_CYCLES;
        after = DMA_NESTED_RETURN_CYCLES;
    }
    if (Psyz_MdecTrace()) {
        fprintf(stderr, "mdec irq host %d at %llu clocks\n", dma_irq_host,
                (unsigned long long)Psyz_KernelCpuClocks());
    }
    Psyz_KernelDeviceIrq(callback, before, after, DMA_DISPATCH_EXIT_CLOCKS);
}

void Psyz_DmaComplete(unsigned channel) {
    if (channel >= 7)
        return;
    Psyz_KernelLockInterrupts();
    dma_pending |= 1u << channel;
    if (dma_dispatching) {
        Psyz_KernelUnlockInterrupts();
        return;
    }
    dma_dispatching = 1;
    while (dma_pending) {
        unsigned next = 0;
        while (!(dma_pending & (1u << next)))
            ++next;
        dma_pending &= ~(1u << next);
        void (*callback)(void) = dma_callbacks[next];
        if (callback)
            DispatchDma(next, callback);
    }
    dma_dispatching = 0;
    Psyz_KernelUnlockInterrupts();
}

void Psyz_DmaIrqComplete(unsigned channel, int host) {
    Psyz_KernelLockInterrupts();
    int outer = dma_irq_host;
    dma_irq_host = host;
    Psyz_DmaComplete(channel);
    dma_irq_host = outer;
    Psyz_KernelUnlockInterrupts();
}

int InitTAP(char* bufA, long lenA, char* bufB, long lenB) {
    LOG_ONCE("forwarding to InitPAD");
    return InitPAD(bufA, lenA, bufB, lenB);
}

void StartTAP(void) {
    LOG_ONCE("forwarding to StartPAD");
    StartPAD();
}

/* libpad (PadInitDirect .. PadSetMainMode): libpad.c */
