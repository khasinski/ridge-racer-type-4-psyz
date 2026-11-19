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
// When a list given at now_ns starts (DrawOTag), with the previous
// transfer done at dma_free_ns and the GPU busy until gpu_end_ns; *cpu_waits
// is set when the CPU waits for that (an idle queue, the GPU still busy).
uint64_t Psyz_GpuListStartNs(uint64_t now_ns, uint64_t dma_free_ns,
                             uint64_t gpu_end_ns, int* cpu_waits);
// Whether the GPU's DMA holds the bus at at_ns: 1 in a slice (the CPU
// stalled), 2 in a halt between slices, with its end in *until_ns.
int Psyz_GpuDmaAtNs(uint64_t at_ns, uint64_t* until_ns);
// GPU ticks of GP0 commands (DuckStation's costs), updating the draw state.
uint64_t Psyz_GpuCommandTicks(const u_long* words, int n);
#endif
