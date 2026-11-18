// Private header for PSY-Z internal macros
// Not part of the public API - do not include from external code

#ifndef PSYZ_INTERNAL_H
#define PSYZ_INTERNAL_H

#include <stdint.h>

#define INCLUDE_ASM(path, func)
#define WEAK_INCLUDE_ASM(path, func)
#define NOP

#ifndef LEN
#define LEN(x) ((s32)(sizeof(x) / sizeof(*(x))))
#endif

#ifndef LENU
#define LENU(x) ((u32)(sizeof(x) / sizeof(*(x))))
#endif

#ifndef CLAMP
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif

#define VRAM_W 1024
#define VRAM_H 512

// SDL platform lifecycle code uses this to avoid undoing an explicit game
// audio pause when the application returns from the background.
int Psyz_AudioIsPaused(void);

// Emulated time (us) shared by the root counters and the CD drive.
uint64_t Psyz_KernelNowUs(void);
// 1 when that time is the offline sample clock (PSYZ_AUDIO_CLOCK=video):
// it advances only as samples are mixed, never by itself.
int Psyz_KernelOfflineClock(void);

// Offline sample clock and root-counter deadlines; no host-time advancement.
unsigned Psyz_KernelAudioBatch(unsigned maximum);
void Psyz_KernelAdvanceAudio(unsigned frames);
void Psyz_KernelPauseHostClock(int paused);
void Psyz_KernelSetHostClockSpeed(unsigned speed);

// Discard prefetched external CD input without resetting SPU voices/reverb.
void Psyz_SpuFlushCdSamples(void);

// Delivers a kernel event as an emulated device interrupt would: right away,
// or at ExitCriticalSection when the caller is inside a critical section.
void Psyz_KernelRaise(unsigned int desc, unsigned int spec);

// Runs fn as an interrupt on the kernel's interrupt thread. Never blocks, so
// the audio thread can use it.
void Psyz_KernelPost(void (*fn)(void));

// Runs fn as an interrupt once the emulated clock reaches at_us (a device
// completing after its PS1 time); Psyz_KernelCancel drops a pending one.
// A callback has one deadline: posting it again moves it.
void Psyz_KernelPostAt(uint64_t at_us, void (*fn)(void));
void Psyz_KernelCancel(void (*fn)(void));

/* Host-side state the SDK's initialisation resets, besides the PS1 state the
 * library itself clears:
 *   ResetCallback (with no callbacks running: at boot, after StopCallback):
 *     the fractions of time carried between spends (kernel.c)
 *   SpuInit: a running SPU DMA ends and its posted end is dropped (spu.c)
 *   CdInit: the StGetNext poll's clock fraction (libcd.c)
 *   InitCARD/StartCARD: the card driver's requests and posted frame ends
 *   DecDCTReset: a pending MDEC output transfer
 * The environment knobs are read once, at their first use, and keep that
 * value for the process: PSYZ_AUDIO_CLOCK (kernel.c; sdl3_audio.c reads it
 * again at each Psyz_AudioInit, so set it before the first PsyZ call and do
 * not change it), PSYZ_SPU_TIMING, PSYZ_CD_TIMING, PSYZ_CARD_TIMING (until
 * the Psyz_*SetInstantTiming setter), PSYZ_CD_TRACE, PSYZ_CARD_TRACE,
 * PSYZ_SPU_KEY_TRACE, PSYZ_SPU_REG_TRACE, PSYZ_CARD_UNPLUG and
 * PSYZ_CARD_UNFORMATTED (once per slot, until Psyz_CardSetSlotState), and
 * the renderer's (README.md). */

// Completes a native synchronous DMA transfer without recursive callbacks.
void Psyz_DmaComplete(unsigned channel);

// Raises the vblank interrupt; VSync calls it once per emulated frame.
void Psyz_KernelVBlank(void);

// Delivers the sectors a callback-driven CD read has reached (libcd.c).
void Psyz_CdServiceDrive(void);
// The same for a game polling the drive in a busy wait: each poll spends
// the PS1 time of the call (StGetNext's ring poll, or another query).
void Psyz_CdPollDrive(void);
void Psyz_CdPollQuery(void);

// Offline clock only: mixes `us` of emulated time without a VBlank, for a
// native busy wait on a device that runs on that clock. Returns 0 (and does
// nothing) on a real-time clock, where the time passes by itself.
int Psyz_AudioWaitUs(uint64_t us);
// Offline clock: how long after the latest blank's sample boundary the
// console's blank came (its field lengths are not whole samples).
uint64_t Psyz_AudioBlankLagNs(void);
// A blank that came due during such a wait (offline clock, SDL platform):
// counts it in VSync(-1) and runs its VSync work (Psyz_VSyncBlank).
void Psyz_VideoWaitBlank(void);
// The CRTC's vertical display range [y1, y2) became [new_y1, new_y2): moves
// the next blank of the offline clock (sdl3_audio.c).
void Psyz_AudioDisplayRangeChanged(
    unsigned y1, unsigned y2, unsigned new_y1, unsigned new_y2);
// The display mode's PAL bit: the length of the offline clock's fields.
void Psyz_AudioVideoModeChanged(int pal);
// A GPU reset (GP1(00h)): the CRTC's display range returns to its default.
void Psyz_GpuCrtcReset(void);
// The console's region (SetVideoMode): a GPU reset's display mode (libgpu.c).
void Psyz_GpuConsoleVideoMode(int pal);
// The CRTC field (libgpu.c): GPUSTAT bit 31 `line` lines after a blank;
// whether the display is 480-line interlace; the lines of a field (PAL or
// NTSC) and from a blank to the display range; a blank's field change.
int Psyz_GpuCrtcOddLine(unsigned line);
int Psyz_GpuCrtcInterlaced480(void);
unsigned Psyz_GpuCrtcFieldLines(void);
unsigned Psyz_GpuCrtcBlankLines(void);
void Psyz_GpuCrtcBlank(void);
// The work of one blank (libapi.c): pad refresh, the VSync hook, the
// vblank interrupt.
void Psyz_VSyncBlank(void);
// PS1 time (system clocks) of interrupt handler work: the main thread
// loses it in the next CPU work it spends (Psyz_SpendCycles) or, for the
// vblank handler, when VSync returns (kernel.c). Take returns the time owed
// and clears it (a busy wait absorbs it).
void Psyz_KernelInterruptCycles(unsigned cycles);
unsigned Psyz_KernelTakeInterruptCycles(void);
// Whether a blank at blank_ns came while the exception of another
// interrupt was dispatched, after its BIOS chains tested for a vblank
// (test_clocks after it began) and before libetc's dispatcher last read
// the interrupt status (exit_clocks before its handlers end): libetc then
// takes that vblank and the chain handlers miss it. Offline clock only.
int Psyz_KernelBlankInDispatch(uint64_t blank_ns, unsigned test_clocks,
                               unsigned exit_clocks);
// The same, for a blank at most `until` clocks after the exception began.
int Psyz_KernelBlankInDispatchUpTo(uint64_t blank_ns, unsigned test_clocks,
                                   unsigned exit_clocks, unsigned until);
// An interrupt the kernel does not dispatch itself (a device model's: CD,
// DMA) began at at_ns and its exception takes `cycles` (the window of
// Psyz_KernelBlankInDispatch); `charged` of them become interrupt handler
// time (the rest is spent elsewhere, e.g. inside a wait that counts it).
// Offline clock only.
void Psyz_KernelDispatch(uint64_t at_ns, unsigned cycles, unsigned charged);
// The CPU stalls `clocks` at at_ns (a DMA moving data): owed to the main
// thread from then on, like a handler's time. Offline clock only.
void Psyz_KernelStallAt(uint64_t at_ns, unsigned clocks);
// The offline clock's position in ns (the time spent but not yet mixed
// included; not the handlers' time owed).
uint64_t Psyz_KernelNowNs(void);
// PSYZ_PAD_TRACE: clocks from the latest such exception to at_ns, and
// its handlers' clocks; 0 when there was none before.
int Psyz_KernelLastDispatch(uint64_t at_ns, unsigned* since_clocks,
                            unsigned* cycles);
// libpad (libpad.c): its handler runs at each vblank once PadStartCom ran;
// Psyz_LibpadVBlank is that handler, and Psyz_LibpadExchanged tells whether
// a port's controller answered at it and the halfwords of the packet it
// received to the end (0 when the exchange failed).
int Psyz_PadDriverRunning(void);
// The BIOS card driver: its handlers are in the interrupt chains
// (InitCARD), and it runs at each vblank (StartCARD .. StopCARD).
int Psyz_CardChainInstalled(void);
int Psyz_CardDriverRunning(void);
void Psyz_LibpadVBlank(void);
int Psyz_LibpadExchanged(int port, int* halfwords);
// The controllers' side of the serial exchange (pad.c): the byte a
// controller answers to @p in with the input of @p frame, and whether it
// ACKs it; deselecting the port ends its command.
int Psyz_PadsTransfer(int port, const char* frame, unsigned char in,
                      unsigned char* out);
void Psyz_PadsDeselect(int port);
// The frame a port's controller answers with at this blank (libapi.c).
const char* Psyz_PadsFrame(int port);

// The BIOS memory card driver's work at a vblank (libcard.c); turn 0: a
// vblank its handler misses.
void Psyz_CardVBlank(int turn);
// The blank where a raw card access ends: libpad has no exchange there.
int Psyz_CardRawEndsAt(int turn);
// A BIOS file service that reads a card sector itself and waits for it
// without raising events (firstfile reads sector 0).
void Psyz_CardSyncTransfer(long chan);

// Services due timers when the host has no thread to run them.
void Psyz_KernelPoll(void);
void Psyz_KernelLockInterrupts(void);
void Psyz_KernelUnlockInterrupts(void);

// Subpixel GTE->GPU coordinate cache for optional geometry correction.
/* Bounded diagnostic only; no cache entries, addresses or rendering policy altered. */
typedef struct {
    unsigned long long matrix_high, matrix_integer, xy_clip, xy_disagree;
    unsigned long long lookup_hit, lookup_miss, copy_hit, copy_miss;
    unsigned samples;
    struct { float expected_x, expected_y, actual_x, actual_y, depth; unsigned reason; } sample[2];
} PsyzPgxpReasons;
void Psyz_PgxpTraceReasonsEnable(int enabled);
void Psyz_PgxpTraceReasonsTake(PsyzPgxpReasons *out);
void Psyz_PgxpSetActive(int active);
void Psyz_PgxpNextFrame(void);
void Psyz_PgxpSetCulling(int enabled);
void Psyz_PgxpSetEffectEnabled(int enabled);
void Psyz_GteCopyQueuedPgxpSxy(
    const void* src, const void* dst, unsigned int sxy);
int Psyz_PgxpLookupVertex(const void* packet_sxy_addr, unsigned int sxy,
                          float* x, float* y, float* w);

#endif
