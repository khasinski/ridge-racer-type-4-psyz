#ifndef PSYZ_GPU_TIMING_H
#define PSYZ_GPU_TIMING_H
#include <stdint.h>
#include <psyz.h>

/* The GPU's drawing time and its list DMA on the emulated clock
 * (gpu_timing.c). A transfer: Begin, a Node per DMA node with its GP0
 * words, End. */
void Psyz_GpuTimingBegin(void);
void Psyz_GpuTimingNode(const u_long* words, int n);
void Psyz_GpuTimingEnd(void);
// A VRAM upload's DMA (LoadImage), after the GPU's work before it.
void Psyz_GpuTimingImage(unsigned words);
// When the GPU and its DMA have finished what they were given.
uint64_t Psyz_GpuTimingIdleNs(void);
void Psyz_GpuTimingReset(void);
// DrawSync(0): the CPU waits until then.
void Psyz_GpuTimingWait(void);
// GPU ticks of GP0 commands (DuckStation's costs), updating the draw state.
uint64_t Psyz_GpuCommandTicks(const u_long* words, int n);
#endif
