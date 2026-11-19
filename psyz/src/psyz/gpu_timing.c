/* The GPU's drawing time and the list DMA that feeds it, on the emulated
 * clock, as DuckStation models them (src/core/gpu.cpp, dma.cpp at the
 * reference commit, tools/port/DUCKSTATION.md of the game):
 *
 * - Each GP0 command costs GPU ticks (two pixels a system clock): polygon
 *   setup by kind, the triangles' area (clamped to the drawing area; twice
 *   for textured, +50% for semi-transparent or mask-checked drawing, half in
 *   interlaced 480-line rendering that skips the displayed field), a
 *   rectangle's rows by texture mode, a line's longer side, a fill's
 *   46 + (w / 8 + 9) * h, a VRAM copy's 2 * w * h, a CLUT reload's 16 or
 *   256, an environment command's 1.
 * - The GPU runs at most 128 GPU ticks ahead: a command waits in the FIFO
 *   until the work before it is down to that, and the list DMA moves on
 *   only once the FIFO has taken the command (so it pushes one command
 *   ahead of the drawing). The DMA stalls the CPU while it moves data: 8
 *   clocks a node header, 5 more and the RAM's 17 per 16 words a packet,
 *   in 1000-clock slices with 100 clocks for the CPU between them.
 * - The next transfer (a DrawOTag queued behind another) starts when the
 *   previous one is done; with none in progress libgpu waits for the GPU
 *   to go idle before it starts one, as DrawSync(0) does.
 *
 * The CPU's stalls are owed at the times the DMA makes them
 * (Psyz_KernelStallAt), so a frame whose GPU is still busy with the work
 * before its DrawOTag loses the DMA's time later, while the GPU takes the
 * list, as on the console. Offline clock only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <psyz/system.h>
#include "../internal.h"
#include "../timing_profile.h"
#include "gpu_timing.h"

enum {
    RUN_AHEAD_GPU_TICKS = 128,
    DMA_SLICE_CLOCKS = 1000,
    DMA_HALT_CLOCKS = 100,
    MAX_PRIM_W = 1024,
    MAX_PRIM_H = 512,
};

static struct {
    int active;              // timing a transfer
    uint64_t dma_ns;         // where the DMA has got to
    uint64_t gpu_end_ns;     // when the GPU's accepted work is done
    uint64_t dma_free_ns;    // when the last transfer ended
    unsigned slice;          // stall clocks in the current DMA slice
    // GPU state the costs depend on
    int area_x0, area_y0, area_x1, area_y1; // clamped drawing area (x1, y1 exclusive)
    int ofs_x, ofs_y;
    unsigned texmode;  // 0 4-bit, 1 8-bit, 2/3 direct
    int draw_to_display;
    int check_mask;
    unsigned clut;
    int clut_8bit;
    int clut_valid;
    unsigned e3, e4;
    int masked;            // _addque2 waited: 1 in its first slice, 2 after
    uint64_t first_end_ns; // where that slice ended
} g = {0, 0, 0, 0, 0, 0, 0, 1024, 512, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static uint64_t clocks_ns(uint64_t clocks) { return clocks * 1000000000u / 33868800u; }

static void set_area(void) {
    int left = g.e3 & 0x3FF, top = (g.e3 >> 10) & 0x3FF;
    int right = g.e4 & 0x3FF, bottom = (g.e4 >> 10) & 0x3FF;
    if (left > right || top > bottom) {
        g.area_x0 = g.area_y0 = g.area_x1 = g.area_y1 = 0;
        return;
    }
    g.area_x1 = right + 1 < 1024 ? right + 1 : 1024;
    g.area_x0 = left < right ? left : right;
    if (g.area_x0 > 1023) g.area_x0 = 1023;
    g.area_y1 = bottom + 1 < 512 ? bottom + 1 : 512;
    g.area_y0 = top < bottom ? top : bottom;
    if (g.area_y0 > 511) g.area_y0 = 511;
}

static int sext11(unsigned v) { return (int)((v & 0x7FF) ^ 0x400) - 0x400; }

static int skip_field(void) {
    return Psyz_GpuCrtcInterlaced480() && !g.draw_to_display;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint64_t tri_ticks(int x1, int y1, int x2, int y2, int x3, int y3, int textured, int semi) {
    x1 = clampi(x1, g.area_x0, g.area_x1); y1 = clampi(y1, g.area_y0, g.area_y1);
    x2 = clampi(x2, g.area_x0, g.area_x1); y2 = clampi(y2, g.area_y0, g.area_y1);
    x3 = clampi(x3, g.area_x0, g.area_x1); y3 = clampi(y3, g.area_y0, g.area_y1);
    int64_t twice = (int64_t)x1 * y2 + (int64_t)x2 * y3 + (int64_t)x3 * y1 -
                    (int64_t)x1 * y3 - (int64_t)x2 * y1 - (int64_t)x3 * y2;
    int64_t pixels = twice / 2;
    if (pixels < 0) pixels = -pixels;
    if (textured) pixels += pixels;
    if (semi || g.check_mask) pixels += (pixels + 1) / 2;
    if (skip_field()) pixels /= 2;
    return (uint64_t)pixels;
}

static int too_large(int xa, int ya, int xb, int yb, int xc, int yc) {
    int minx = xa < xb ? xa : xb, maxx = xa > xb ? xa : xb;
    int miny = ya < yb ? ya : yb, maxy = ya > yb ? ya : yb;
    minx = minx < xc ? minx : xc; maxx = maxx > xc ? maxx : xc;
    miny = miny < yc ? miny : yc; maxy = maxy > yc ? maxy : yc;
    return maxx + 1 - minx > MAX_PRIM_W || maxy + 1 - miny > MAX_PRIM_H;
}

static uint64_t clut_ticks(unsigned clut) {
    if (g.texmode >= 2) return 0;
    int is8 = g.texmode == 1;
    if (!g.clut_valid || clut != g.clut || is8 > g.clut_8bit) {
        g.clut = clut;
        g.clut_8bit = is8;
        g.clut_valid = 1;
        return is8 ? 256 : 16;
    }
    return 0;
}

static uint64_t rect_ticks(int x, int y, int w, int h, int textured, int semi) {
    int x0 = x > g.area_x0 ? x : g.area_x0, y0 = y > g.area_y0 ? y : g.area_y0;
    int x1 = x + w < g.area_x1 ? x + w : g.area_x1, y1 = y + h < g.area_y1 ? y + h : g.area_y1;
    unsigned dw = x1 > x0 ? (unsigned)(x1 - x0) : 0, dh = y1 > y0 ? (unsigned)(y1 - y0) : 0;
    uint64_t row = dw;
    if (textured) {
        switch (g.texmode) {
        case 0:
            row += dw;
            break;
        case 1:
            if (dw > 128) row += (dw / 4) * 8;
            else if (dw * dh > 2048) row += (dw / 4) * (4 * (128 / dw));
            else row += dw;
            break;
        default:
            if (dw > 128) row += (dw / 2) * 8;
            else if (dw * dh > 1024) row += (dw / 4) * (8 * (128 / (dw ? dw : 1)));
            else row += dw;
            break;
        }
    }
    if (semi || g.check_mask) row += (dw + 1) / 2;
    if (skip_field()) dh = dh / 2 > 1 ? dh / 2 : 1;
    return row * dh;
}

static uint64_t line_ticks(int xa, int ya, int xb, int yb) {
    int x0 = xa < xb ? xa : xb, x1 = (xa > xb ? xa : xb) + 1;
    int y0 = ya < yb ? ya : yb, y1 = (ya > yb ? ya : yb) + 1;
    if (x1 - x0 > MAX_PRIM_W || y1 - y0 > MAX_PRIM_H) return 0;
    x0 = x0 > g.area_x0 ? x0 : g.area_x0; y0 = y0 > g.area_y0 ? y0 : g.area_y0;
    x1 = x1 < g.area_x1 ? x1 : g.area_x1; y1 = y1 < g.area_y1 ? y1 : g.area_y1;
    if (x1 <= x0 || y1 <= y0) return 0;
    unsigned w = (unsigned)(x1 - x0), h = (unsigned)(y1 - y0);
    if (skip_field()) h = h / 2 > 1 ? h / 2 : 1;
    return w > h ? w : h;
}

uint64_t Psyz_GpuCommandTicks(const u_long* w, int n) {
    uint64_t ticks = 0;
    for (int i = 0; i < n;) {
        unsigned op = (unsigned)w[i];
        unsigned code = op >> 24;
        if (code >= 0x20 && code < 0x40) {
            int quad = (code & 8) != 0, shaded = (code & 0x10) != 0;
            int textured = (code & 4) != 0, semi = (code & 2) != 0;
            int per = 1 + textured + shaded, verts = quad ? 4 : 3;
            int total = per * verts + !shaded;
            static const unsigned setup[2][2][2] = {{{46, 226}, {334, 496}}, {{82, 262}, {370, 532}}};
            if (i + total > n) break;
            ticks += setup[quad][shaded][textured];
            int x[4], y[4];
            for (int v = 0; v < verts; v++) {
                // [color (shaded, v > 0)] xy [uv]
                int at = shaded ? i + v * per + 1 : i + 1 + v * (1 + textured);
                unsigned xy = (unsigned)w[at];
                x[v] = g.ofs_x + sext11(xy);
                y[v] = g.ofs_y + sext11(xy >> 16);
            }
            if (textured) {
                unsigned uv0 = (unsigned)w[i + 2];
                unsigned uv1 = (unsigned)w[shaded ? i + 5 : i + 4];
                unsigned texpage = uv1 >> 16;
                g.texmode = (texpage >> 7) & 3;
                ticks += clut_ticks(uv0 >> 16);
            }
            if (!too_large(x[0], y[0], x[1], y[1], x[2], y[2]))
                ticks += tri_ticks(x[0], y[0], x[1], y[1], x[2], y[2], textured, semi);
            if (quad && !too_large(x[1], y[1], x[2], y[2], x[3], y[3]))
                ticks += tri_ticks(x[2], y[2], x[1], y[1], x[3], y[3], textured, semi);
            i += total;
        } else if (code >= 0x40 && code < 0x60) {
            int shaded = (code & 0x10) != 0, poly = (code & 8) != 0;
            if (!poly) {
                int total = shaded ? 4 : 3;
                if (i + total > n) break;
                unsigned a = (unsigned)w[i + 1], b = (unsigned)w[i + (shaded ? 3 : 2)];
                ticks += line_ticks(g.ofs_x + sext11(a), g.ofs_y + sext11(a >> 16),
                                    g.ofs_x + sext11(b), g.ofs_y + sext11(b >> 16));
                i += total;
            } else {
                ticks += 16;
                int j = i + 1;
                unsigned prev = (unsigned)w[j++];
                for (;;) {
                    if (j >= n || ((unsigned)w[j] & 0xF000F000) == 0x50005000) {
                        break;
                    }
                    if (shaded) {
                        j++; // the next vertex's color
                        if (j >= n || ((unsigned)w[j] & 0xF000F000) == 0x50005000) {
                            break;
                        }
                    }
                    unsigned cur = (unsigned)w[j++];
                    ticks += line_ticks(g.ofs_x + sext11(prev), g.ofs_y + sext11(prev >> 16),
                                        g.ofs_x + sext11(cur), g.ofs_y + sext11(cur >> 16));
                    prev = cur;
                }
                i = j + 1;
            }
        } else if (code >= 0x60 && code < 0x80) {
            int textured = (code & 4) != 0, semi = (code & 2) != 0;
            int size = (code >> 3) & 3;
            int total = 2 + textured + (size == 0);
            if (i + total > n) break;
            ticks += 16;
            if (textured) ticks += clut_ticks((unsigned)w[i + 2] >> 16);
            unsigned xy = (unsigned)w[i + 1];
            int rw = 1, rh = 1;
            if (size == 2) rw = rh = 8;
            else if (size == 3) rw = rh = 16;
            else if (size == 0) {
                unsigned wh = (unsigned)w[i + 2 + textured];
                rw = wh & 0x3FF;
                rh = (wh >> 16) & 0x1FF;
            }
            int rx = sext11(g.ofs_x + sext11(xy)), ry = sext11(g.ofs_y + sext11(xy >> 16));
            ticks += rect_ticks(rx, ry, rw, rh, textured, semi);
            i += total;
        } else if (code == 0x02) {
            if (i + 3 > n) break;
            unsigned wh = (unsigned)w[i + 2];
            unsigned fw = ((wh & 0x3FF) + 0xF) & ~0xFu, fh = (wh >> 16) & 0x1FF;
            if (fw && fh) ticks += 46 + ((fw / 8) + 9) * fh;
            i += 3;
        } else if (code >= 0x80 && code < 0xA0) {
            if (i + 4 > n) break;
            unsigned wh = (unsigned)w[i + 3];
            unsigned cw = wh & 0x3FF, ch = (wh >> 16) & 0x1FF;
            if (!cw) cw = 0x400;
            if (!ch) ch = 0x200;
            ticks += (uint64_t)cw * ch * 2;
            i += 4;
        } else if (code >= 0xE1 && code <= 0xE6) {
            switch (code) {
            case 0xE1:
                g.texmode = (op >> 7) & 3;
                g.draw_to_display = (op >> 10) & 1;
                break;
            case 0xE3: g.e3 = op; set_area(); break;
            case 0xE4: g.e4 = op; set_area(); break;
            case 0xE5:
                g.ofs_x = sext11(op);
                g.ofs_y = sext11(op >> 11);
                break;
            case 0xE6: g.check_mask = (op >> 1) & 1; break;
            }
            ticks += 1;
            i++;
        } else if (code == 0x01) {
            ticks += 1;
            g.clut_valid = 0;
            i++;
        } else {
            i++; // NOP and what is not drawing
        }
    }
    return ticks;
}

static uint64_t now_ns(void) {
    return Psyz_ClocksToNs(Psyz_EmulatedClocks());
}

/* The first slice of a list _addque2 started after its wait ends before
 * the mask's restore: the masked wait spends it (Psyz_GpuTimingEnd). It
 * ends after DMA_SLICE_CLOCKS or where the DMA waits for the GPU. */
static void end_first_slice(void) {
    if (g.masked == 1) {
        g.masked = 2;
        g.first_end_ns = g.dma_ns;
    }
}

/* The CPU loses `clocks` of DMA at the DMA's position, in slices. */
static void dma_stall(unsigned clocks) {
    while (clocks) {
        unsigned room = DMA_SLICE_CLOCKS - g.slice;
        unsigned take = clocks < room ? clocks : room;
        if (g.masked != 1) Psyz_KernelStallAt(g.dma_ns, take);
        g.dma_ns += clocks_ns(take);
        g.slice += take;
        clocks -= take;
        if (g.slice >= DMA_SLICE_CLOCKS) {
            end_first_slice();
            g.slice = 0;
            g.dma_ns += clocks_ns(DMA_HALT_CLOCKS);
        }
    }
}

static int timing_on(void) { return Psyz_KernelOfflineClock(); }

/* PSYZ_GPU_TRACE=1: each list's start, size and modelled end on stderr. */
static int gpu_trace(void) {
    static int t = -1;
    if (t < 0) {
        const char* v = getenv("PSYZ_GPU_TRACE");
        t = v && *v == '1';
    }
    return t;
}
static unsigned list_nodes, list_words;
static uint64_t list_ticks;

uint64_t Psyz_GpuListStartNs(uint64_t now_ns, uint64_t dma_free_ns,
                             uint64_t gpu_end_ns, int* cpu_waits) {
    // libgpu starts a transfer once the GPU is idle (GPUSTAT bit 26), so
    // the CPU waits for the drawing before it (DuckStation: a movie
    // frame's DrawOTag returns 43k clocks after its call, its PutDrawEnv's
    // fill still drawing; at boot 1.4k, the fill done). While the transfer
    // before is still in progress libgpu queues this one instead and its
    // DMA callback starts it later: only an idle queue waits.
    *cpu_waits = 0;
    if (dma_free_ns > now_ns)
        return gpu_end_ns > dma_free_ns ? gpu_end_ns : dma_free_ns;
    if (gpu_end_ns > now_ns) {
        *cpu_waits = 1;
        return gpu_end_ns;
    }
    return now_ns;
}

void Psyz_GpuTimingBegin(void) {
    if (!timing_on()) return;
    uint64_t now = now_ns();
    g.active = 1;
    int cpu_waits;
    g.dma_ns = Psyz_GpuListStartNs(now, g.dma_free_ns, g.gpu_end_ns, &cpu_waits);
    g.slice = 0;
    /* _addque2 polls for the idle GPU with the interrupts masked, then
     * starts the DMA and restores the mask after its first slice: on
     * DuckStation a CD sector's or the timer's interrupt that comes during
     * a movie frame's 43k-clock DrawOTag is taken then (Versus movie, odd
     * fields: at 93126 clocks after the blank, due from 73.6k). */
    g.masked = 0;
    if (cpu_waits) {
        g.dma_ns += clocks_ns(ADDQUE2_POLL_EXIT_CLOCKS);
        g.masked = 1;
        g.first_end_ns = g.dma_ns;
    }
    list_nodes = list_words = 0;
    list_ticks = 0;
    if (gpu_trace())
        fprintf(stderr, "gpu: list at %llu dma %llu gpu_end %llu\n",
                (unsigned long long)now, (unsigned long long)g.dma_ns,
                (unsigned long long)g.gpu_end_ns);
}

void Psyz_GpuTimingNode(const u_long* words, int n) {
    if (!g.active) return;
    dma_stall(DRAW_OTAG_NODE_CYCLES);
    if (n <= 0) return;
    // the FIFO takes the packet once the GPU is down to its run-ahead
    uint64_t accept = g.gpu_end_ns > clocks_ns(RUN_AHEAD_GPU_TICKS / 2)
                          ? g.gpu_end_ns - clocks_ns(RUN_AHEAD_GPU_TICKS / 2) : 0;
    if (accept > g.dma_ns) {
        end_first_slice();
        g.dma_ns = accept;
        g.slice = 0;
    }
    dma_stall(DRAW_OTAG_BLOCK_CYCLES + (unsigned)n + ((unsigned)n + 15) / 16);
    uint64_t ticks = Psyz_GpuCommandTicks(words, n);
    list_nodes++;
    list_words += (unsigned)n;
    list_ticks += ticks;
    uint64_t start = g.gpu_end_ns > g.dma_ns ? g.gpu_end_ns : g.dma_ns;
    g.gpu_end_ns = start + clocks_ns((ticks + 1) / 2);
}

void Psyz_GpuTimingEnd(void) {
    if (!g.active) return;
    g.active = 0;
    g.dma_free_ns = g.dma_ns;
    if (g.masked) {
        end_first_slice();
        g.masked = 0;
        Psyz_KernelWaitMaskedUntilNs(g.first_end_ns + clocks_ns(ADDQUE2_RESTORE_CLOCKS));
    }
    if (gpu_trace())
        fprintf(stderr, "gpu: end packets %u words %u ticks %llu dma_end %llu gpu_end %llu\n",
                list_nodes, list_words, (unsigned long long)list_ticks,
                (unsigned long long)g.dma_ns, (unsigned long long)g.gpu_end_ns);
}

void Psyz_GpuTimingImage(unsigned words) {
    if (!timing_on()) return;
    uint64_t now = now_ns();
    g.dma_ns = now > g.dma_free_ns ? now : g.dma_free_ns;
    if (g.gpu_end_ns > g.dma_ns) g.dma_ns = g.gpu_end_ns; // A0h waits for the GPU
    g.slice = 0;
    dma_stall(words + (words + 15) / 16);
    g.dma_free_ns = g.dma_ns;
    if (g.gpu_end_ns < g.dma_ns) g.gpu_end_ns = g.dma_ns;
}

uint64_t Psyz_GpuTimingIdleNs(void) {
    return g.gpu_end_ns > g.dma_free_ns ? g.gpu_end_ns : g.dma_free_ns;
}

void Psyz_GpuTimingReset(void) {
    g.gpu_end_ns = g.dma_free_ns = 0;
    g.clut_valid = 0;
}

void Psyz_GpuTimingWait(void) {
    if (!timing_on()) return;
    uint64_t idle = Psyz_GpuTimingIdleNs();
    uint64_t pos = now_ns();
    if (idle > pos) {
        Psyz_SpendTime((unsigned)(idle - pos));
    }
}
