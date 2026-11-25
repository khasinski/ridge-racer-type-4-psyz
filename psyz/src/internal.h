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

// The kernel's time (kernel.c): the host's clock without its pauses and at
// the game's speed, or the offline clock (PSYZ_AUDIO_CLOCK=video): a
// deterministic field clock that stands still while the game works and
// moves only in its waits.
uint64_t Psyz_KernelNowUs(void);
uint64_t Psyz_KernelNowNs(void);
int Psyz_KernelOfflineClock(void);

// Offline clock (kernel.c): the clock moves to at_ns, running the kernel's
// deadlines and the platform's blanks on the way at their times and mixing
// the audio up to each (the platform's waits call it).
void Psyz_KernelAdvanceToNs(uint64_t at_ns);
// A wait until at_ns on either clock: the offline clock moves there, a
// real-time one sleeps while the interrupt thread runs what comes due.
void Psyz_KernelWaitUntilNs(uint64_t at_ns);
void Psyz_KernelPauseHostClock(int paused);
void Psyz_KernelSetHostClockSpeed(unsigned speed);

// Discard prefetched external CD input without resetting SPU voices/reverb.
void Psyz_SpuFlushCdSamples(void);

// Delivers a kernel event as a device interrupt would: right away, or at
// ExitCriticalSection when the caller is inside a critical section.
void Psyz_KernelRaise(unsigned int desc, unsigned int spec);

// Runs fn as an interrupt: on the kernel's interrupt thread, or (offline
// clock) at the main thread's next service. Never blocks, so the audio
// thread can use it.
void Psyz_KernelPost(void (*fn)(void));

// Runs fn as an interrupt once the kernel's clock reaches at_ns (a device
// completing at its time); Psyz_KernelCancel drops a pending one. A
// callback has one deadline: posting it again moves it.
void Psyz_KernelPostAtNs(uint64_t at_ns, void (*fn)(void));
void Psyz_KernelCancel(void (*fn)(void));

/* Host-side state the SDK's initialisation resets, besides the PS1 state the
 * library itself clears:
 *   SpuInit: a running SPU DMA ends and its posted end is dropped (spu.c)
 * The environment knobs are read once, at their first use, and keep that
 * value for the process: PSYZ_AUDIO_CLOCK (kernel.c; sdl3_audio.c reads it
 * again at each Psyz_AudioInit, so set it before the first PsyZ call and do
 * not change it), PSYZ_SPU_KEY_TRACE, PSYZ_SPU_REG_TRACE, PSYZ_CARD_UNPLUG
 * and PSYZ_CARD_UNFORMATTED (once per slot, until Psyz_CardSetSlotState),
 * and the renderer's (README.md). */

// Completes a native synchronous DMA transfer without recursive callbacks.
void Psyz_DmaComplete(unsigned channel);

// Raises the vblank interrupt; VSync calls it once per field.
void Psyz_KernelVBlank(void);

// Delivers what the CD drive has for its callbacks (libcd.c).
void Psyz_CdServiceDrive(void);

// Offline clock only: moves the clock `ns` on, for a wait; blanks it
// crosses happen in it. Returns 0 (and does nothing) on a real-time clock
// or while the clock is paused.
int Psyz_AudioWaitNs(uint64_t ns);
// The platform's side of the offline clock (Psyz_KernelAdvanceToNs): mixes
// the samples due by at_ns; the time of the next blank (UINT64_MAX while a
// VSync waits for it itself); the blank that time has reached.
void Psyz_AudioMixTo(uint64_t at_ns);
uint64_t Psyz_AudioNextBlankNs(void);
void Psyz_AudioBlankDue(void);
// A blank that came due during such a wait (offline clock, SDL platform):
// counts it in VSync(-1) and runs its VSync work (Psyz_VSyncBlank).
void Psyz_VideoWaitBlank(void);
// Real-time clock: the host time to the field clock's next blank (the one
// VSync(0) waits for), UINT64_MAX when the fields are not paced by it.
uint64_t Psyz_VideoNsUntilBlank(void);
// The display mode's PAL bit: the length of the offline clock's fields.
void Psyz_AudioVideoModeChanged(int pal);
// A GPU reset (GP1(00h)): the display mode returns to the console's.
void Psyz_GpuCrtcReset(void);
// The console's region (SetVideoMode): a GPU reset's display mode (libgpu.c).
void Psyz_GpuConsoleVideoMode(int pal);
// The lines of a field (PAL or NTSC) of the display mode (libgpu.c).
unsigned Psyz_GpuCrtcFieldLines(void);
// ... and those outside the vertical display range (the blank's).
unsigned Psyz_GpuCrtcBlankLines(void);
// The work of one blank (libapi.c): pad refresh, the VSync hook, the
// vblank interrupt.
void Psyz_VSyncBlank(void);
// The blocking VSync calls so far (libapi.c): the game's frames, for the
// display model's log (PSYZ_SIM_DISPLAY).
unsigned Psyz_VSyncWaits(void);
// libpad (libpad.c): its handler runs at each vblank once PadStartCom ran;
// Psyz_LibpadVBlank is that handler.
int Psyz_PadDriverRunning(void);
void Psyz_LibpadVBlank(void);
// The controllers' side of the serial exchange (pad.c): the byte a
// controller answers to @p in with the input of @p frame, and whether it
// ACKs it; deselecting the port ends its command.
int Psyz_PadsTransfer(int port, const char* frame, unsigned char in,
                      unsigned char* out);
void Psyz_PadsDeselect(int port);
// The frame a port's controller answers with at this blank (libapi.c).
const char* Psyz_PadsFrame(int port);

// Applies Psyz_CardSetSlotStateAtBlank's state at the start of a blank.
void Psyz_CardBlankBegin(void);

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
/* As Psyz_PgxpLookupVertex; *flags: bit 0 effect, bit 1 no depth
 * (Psyz_GteSetPgxpDepthClass), bit 3 a layer whose ordering bias is the
 * signed byte of bits 8..15 (Psyz_GteSetPgxpDepthLayer), bit 4 a depth
 * only (Psyz_GteStorePgxpDepth), bit 5 a decal (PSYZ_PGXP_DEPTH_DECAL). */
int Psyz_PgxpLookupVertexFlags(const void* packet_sxy_addr, unsigned int sxy,
                               float* x, float* y, float* w,
                               unsigned int* flags, float* depth_w);

#endif
