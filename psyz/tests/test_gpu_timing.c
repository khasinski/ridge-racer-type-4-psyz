#include "ztest.h"
#include <psyz.h>
#include "../src/psyz/gpu_timing.h"

/* GPU command costs, as DuckStation's gpu.cpp charges them (GPU ticks). */

static uint64_t ticks(const u_long* w, int n) { return Psyz_GpuCommandTicks(w, n); }

static void full_area(void) {
    // drawing area 0,0 .. 1023,511, offset 0, 4-bit textures, no mask test
    u_long env[] = {0xE1000000, 0xE3000000, 0xE4000000 | (511u << 10) | 1023u,
                    0xE5000000, 0xE6000000, 0x01000000};
    ticks(env, 6);
}

ZTEST_SETUP(gpu_timing) { full_area(); }
ZTEST_TEARDOWN(gpu_timing) { full_area(); }

ZTEST(gpu_timing, environment_commands_take_a_tick) {
    u_long w[] = {0xE1000000, 0xE5000000};
    zexpect_s32_eq(2, (int)ticks(w, 2));
}

ZTEST(gpu_timing, flat_triangle_is_setup_plus_area) {
    u_long w[] = {0x20FFFFFF, 0x00000000, 0x0000000A, 0x000A0000};
    zexpect_s32_eq(46 + 50, (int)ticks(w, 4));
}

ZTEST(gpu_timing, textured_semi_transparent_triangle) {
    // 4-bit texpage in the second vertex's uv word, a CLUT reload (16)
    u_long w[] = {0x26808080, 0x00000000, 0x7FC00000, 0x0000000A,
                  0x00000000, 0x000A0000, 0x00000000};
    zexpect_s32_eq(226 + 16 + 100 + 50, (int)ticks(w, 7));
    // the same CLUT again: no reload
    zexpect_s32_eq(226 + 100 + 50, (int)ticks(w, 7));
}

ZTEST(gpu_timing, triangle_is_clamped_to_the_drawing_area) {
    u_long area[] = {0xE3000000, 0xE4000000 | (4u << 10) | 4u}; // 0,0 .. 4,4
    ticks(area, 2);
    u_long w[] = {0x20FFFFFF, 0x00000000, 0x00000064, 0x00640000};
    zexpect_s32_eq(46 + 12, (int)ticks(w, 4)); // clamped to 0..5: area 12.5
}

ZTEST(gpu_timing, fill_and_copy) {
    u_long fill[] = {0x02000000, 0x00000000, (240u << 16) | 320u};
    zexpect_s32_eq(46 + (40 + 9) * 240, (int)ticks(fill, 3));
    u_long copy[] = {0x80000000, 0x00000000, 0x00100010, 0x00100010};
    zexpect_s32_eq(16 * 16 * 2, (int)ticks(copy, 4));
}

ZTEST(gpu_timing, rectangles_by_size) {
    u_long r16[] = {0x78FFFFFF, 0x00000000};
    zexpect_s32_eq(16 + 16 * 16, (int)ticks(r16, 2));
    u_long var[] = {0x60FFFFFF, 0x00000000, (10u << 16) | 20u};
    zexpect_s32_eq(16 + 20 * 10, (int)ticks(var, 3));
}

ZTEST(gpu_timing, line_takes_its_longer_side) {
    u_long w[] = {0x40FFFFFF, 0x00000000, 0x00030014};
    zexpect_s32_eq(21, (int)ticks(w, 3));
}


ZTEST(gpu_timing, a_list_waits_for_an_idle_gpu) {
    int waits;
    // the GPU idle: at once
    zexpect_s32_eq(1000, (int)Psyz_GpuListStartNs(1000, 500, 800, &waits));
    zexpect_s32_eq(0, waits);
    // still drawing: the CPU waits for it (libgpu polls GPUSTAT)
    zexpect_s32_eq(5000, (int)Psyz_GpuListStartNs(1000, 500, 5000, &waits));
    zexpect_s32_eq(1, waits);
    // a transfer in progress: queued behind it, the CPU goes on
    zexpect_s32_eq(5000, (int)Psyz_GpuListStartNs(1000, 3000, 5000, &waits));
    zexpect_s32_eq(0, waits);
    zexpect_s32_eq(3000, (int)Psyz_GpuListStartNs(1000, 3000, 2000, &waits));
    zexpect_s32_eq(0, waits);
}
