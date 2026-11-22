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
 * - The GPU's command event, its FIFO and the list DMA as DuckStation
 *   schedules them (below, before Psyz_GpuTimingBegin): the DMA stalls
 *   the CPU while it moves data, 8 clocks a node header, 5 more and the
 *   RAM's 17 per 16 words a packet, in 1000-clock slices with 100 clocks
 *   for the CPU between them, and only while the GPU's FIFO is empty.
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
} g = {.area_x1 = 1024, .area_y1 = 512};


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

static void CommandTicksOne(const u_long* w, int n, int* pi, uint64_t* pticks);

/* The ticks of each command in w (each[k], up to max of them, *count). */
static uint64_t CommandTicks(const u_long* w, int n, uint32_t* each, int* count, int max) {
    uint64_t ticks = 0;
    int k = 0;
    for (int i = 0; i < n;) {
        uint64_t before = ticks;
        int at = i;
        CommandTicksOne(w, n, &i, &ticks);
        if (i == at) break; // incomplete
        if (each && k < max) each[k++] = (uint32_t)(ticks - before);
    }
    if (count) *count = k;
    return ticks;
}

uint64_t Psyz_GpuCommandTicks(const u_long* w, int n) {
    return CommandTicks(w, n, NULL, NULL, 0);
}

/* One command at w[*pi]: its ticks added to *pticks, *pi past it (left
 * where it was when the command is incomplete). */
static void CommandTicksOne(const u_long* w, int n, int* pi, uint64_t* pticks) {
    int i = *pi;
    uint64_t ticks = *pticks;
    {
        unsigned op = (unsigned)w[i];
        unsigned code = op >> 24;
        if (code >= 0x20 && code < 0x40) {
            int quad = (code & 8) != 0, shaded = (code & 0x10) != 0;
            int textured = (code & 4) != 0, semi = (code & 2) != 0;
            int per = 1 + textured + shaded, verts = quad ? 4 : 3;
            int total = per * verts + !shaded;
            static const unsigned setup[2][2][2] = {{{46, 226}, {334, 496}}, {{82, 262}, {370, 532}}};
            if (i + total > n) goto out;
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
                if (i + total > n) goto out;
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
            if (i + total > n) goto out;
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
            if (i + 3 > n) goto out;
            unsigned wh = (unsigned)w[i + 2];
            unsigned fw = ((wh & 0x3FF) + 0xF) & ~0xFu, fh = (wh >> 16) & 0x1FF;
            if (fw && fh) ticks += 46 + ((fw / 8) + 9) * fh;
            i += 3;
        } else if (code >= 0x80 && code < 0xA0) {
            if (i + 4 > n) goto out;
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
    *pi = i;
    *pticks = ticks;
out:;
}

static uint64_t now_ns(void) {
    return Psyz_ClocksToNs(Psyz_EmulatedClocks());
}

static int timing_on(void) { return Psyz_KernelOfflineClock(); }

/* PSYZ_GPU_TRACE=1: each list's start, size and modelled end on stderr;
 * 2 also each command the GPU starts (GPUC), its going idle (GPUI) and
 * each DMA slice (DMAS), as DuckStation's DUCKSTATION_GPU_TRACE and
 * DUCKSTATION_DMA_TRACE, in clocks; 3 also each packet's words.
 * PSYZ_GPU_TRACE_FROM=<clocks>: from that clock on. */
static int gpu_trace_level(void) {
    static int t = -1;
    static unsigned long long from;
    if (t < 0) {
        const char* v = getenv("PSYZ_GPU_TRACE");
        const char* f = getenv("PSYZ_GPU_TRACE_FROM");
        t = v && *v >= '1' && *v <= '9' ? *v - '0' : 0;
        from = f ? strtoull(f, NULL, 10) : 0;
    }
    if (t && from && Psyz_EmulatedClocks() < from) return 0;
    return t;
}
static int gpu_trace(void) { return gpu_trace_level() >= 1; }
static unsigned list_nodes, list_words;
static uint64_t list_ticks;

/* DuckStation's GPU command event and list DMA (gpu.cpp, dma.cpp at the
 * reference commit), on the CPU's clock in system clocks:
 *
 * - pending: the GPU ticks of the commands started that the command event
 *   has not yet counted off. It is counted off only when the event runs,
 *   by twice the clocks since its last run; a command starts while
 *   pending <= RUN_AHEAD_GPU_TICKS, else it waits in the FIFO.
 * - The event is due when pending is down to 0 (pending / 2 clocks on).
 *   A FIFO write outside the event (a DMA slice the CPU started or one
 *   after a halt) reschedules it from the write with the pending not yet
 *   counted off, so a GPU still drawing is due later than its work; the
 *   event then counts the whole time since its last run, and the commands
 *   it starts have that time to their credit.
 * - The DMA moves nodes while the FIFO is empty (a command waiting there
 *   stops the transfer, its empty nodes too), in slices of
 *   DMA_SLICE_CLOCKS (a node is not split) with DMA_HALT_CLOCKS between
 *   them; stopped, it goes on in the command event that empties the FIFO.
 *   Within a slice the GPU sees each node at the slice's start plus the
 *   clocks moved before it.
 * - A GPUSTAT read (libgpu's waits for the idle GPU: DrawSync, _addque2)
 *   runs the event once its pending is due by the clock, so a polled GPU
 *   is idle when its work is done.
 *
 * DuckStation: a main menu field (options, DuckStation frame 1702): a
 * sprite of 20496 ticks every 10249 clocks, the DMA's 17 clocks for the
 * next one in each event; the next node's write after 5k clocks of empty
 * nodes puts the event 5.5k clocks past the sprite's end. */
enum { FIFO_COMMANDS = 1024, NODE_COMMANDS = 256 };
static struct {
    int64_t pending;
    uint64_t last_run, next_run;
    int active;
    int idle;
    uint64_t idle_at;
    uint32_t fifo[FIFO_COMMANDS];
    unsigned head, len;
    // the list DMA
    uint64_t slice_at;
    unsigned used;
    int in_event;
    int masked;            // _addque2 waited: 1 in its first slice, 2 after
    uint64_t first_end;    // where that slice ended
    uint64_t dma_free;     // the last transfer's end
} d = {0, 0, 0, 0, 1, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0};

static uint64_t ns_of(uint64_t clocks) { return Psyz_ClocksToNs(clocks); }

static void gpu_execute(uint64_t t) {
    while (d.pending <= RUN_AHEAD_GPU_TICKS && d.len) {
        uint32_t ticks = d.fifo[d.head];
        d.head = (d.head + 1) % FIFO_COMMANDS;
        d.len--;
        d.pending += ticks;
        if (gpu_trace_level() >= 2)
            fprintf(stderr, "GPUC ticks %u pending %lld tick %llu\n", ticks,
                    (long long)d.pending, (unsigned long long)t);
    }
    int idle = d.pending <= 0 && !d.len;
    if (idle && !d.idle) {
        d.idle_at = t;
        if (gpu_trace_level() >= 2)
            fprintf(stderr, "GPUI tick %llu\n", (unsigned long long)t);
    }
    d.idle = idle;
}

// UpdateCommandTickEvent at ts
static void gpu_schedule(uint64_t ts) {
    if (d.pending <= 0) {
        d.pending = 0;
        d.active = 0;
        return;
    }
    if (!d.active) {
        d.active = 1;
        d.last_run = ts;
    }
    d.next_run = ts + (d.pending <= 1 ? 1 : (uint64_t)((d.pending + 1) >> 1));
}

// the command event's run at t (the caller schedules it again)
static void gpu_fire(uint64_t t) {
    d.pending -= 2 * (int64_t)(t - d.last_run);
    d.last_run = t;
    gpu_execute(t);
}

// the events due by t
static void gpu_run_until(uint64_t t) {
    while (d.active && d.next_run <= t) {
        uint64_t at = d.next_run;
        gpu_fire(at);
        gpu_schedule(at);
    }
}

// When a polled GPU goes idle (GPUSTAT bit 26); the state is kept.
static uint64_t gpu_polled_idle(void) {
    if (d.idle) return d.idle_at;
    int64_t pending = d.pending;
    uint64_t last = d.last_run;
    unsigned head = d.head, len = d.len;
    for (;;) {
        uint64_t t = last + (pending > 0 ? (uint64_t)((pending + 1) >> 1) : 0);
        pending -= 2 * (int64_t)(t - last);
        last = t;
        while (pending <= RUN_AHEAD_GPU_TICKS && len) {
            pending += d.fifo[head];
            head = (head + 1) % FIFO_COMMANDS;
            len--;
        }
        if (pending <= 0 && !len) return t;
    }
}

// libgpu's wait found the GPU idle at t
static void gpu_set_idle(uint64_t t) {
    d.pending = 0;
    d.active = 0;
    d.len = 0;
    if (!d.idle || d.idle_at > t) d.idle_at = t;
    d.idle = 1;
}

static void gpu_push(const uint32_t* ticks, int n, uint64_t ts) {
    for (int k = 0; k < n; k++) {
        if (d.len == FIFO_COMMANDS) { // not on a game's list: fold into the last
            d.fifo[(d.head + d.len - 1) % FIFO_COMMANDS] += ticks[k];
            continue;
        }
        d.fifo[(d.head + d.len) % FIFO_COMMANDS] = ticks[k];
        d.len++;
    }
    if (n) d.idle = 0;
    gpu_execute(ts);
    if (!d.in_event) gpu_schedule(ts);
}

/* The DMA's recent slices and halts, for the MDEC's output DMA that shares
 * them (Psyz_GpuDmaAtNs). */
enum { BUS_SPANS = 64 };
static struct {
    uint64_t from_ns, to_ns;
    int halt;
} bus[BUS_SPANS];
static unsigned bus_next;

static void note_span(uint64_t from_ns, uint64_t to_ns, int halt) {
    unsigned last = (bus_next + BUS_SPANS - 1) % BUS_SPANS;
    if (bus[last].to_ns == from_ns && bus[last].halt == halt && bus[last].to_ns) {
        bus[last].to_ns = to_ns; // the same slice goes on
        return;
    }
    bus[bus_next].from_ns = from_ns;
    bus[bus_next].to_ns = to_ns;
    bus[bus_next].halt = halt;
    bus_next = (bus_next + 1) % BUS_SPANS;
}

int Psyz_GpuDmaAtNs(uint64_t at_ns, uint64_t* until_ns) {
    for (unsigned k = 1; k <= BUS_SPANS; k++) {
        unsigned i = (bus_next + BUS_SPANS - k) % BUS_SPANS;
        if (!bus[i].to_ns) {
            break;
        }
        if (at_ns >= bus[i].from_ns && at_ns < bus[i].to_ns) {
            *until_ns = bus[i].to_ns;
            return bus[i].halt ? 2 : 1;
        }
    }
    return 0;
}

/* The CPU loses `clocks` of DMA at the slice's position. */
static void dma_stall(unsigned clocks) {
    uint64_t at = d.slice_at + d.used;
    if (d.masked != 1) Psyz_KernelStallAt(ns_of(at), clocks);
    note_span(ns_of(at), ns_of(at + clocks), 0);
    d.used += clocks;
}

static void dma_start_slice(uint64_t at, int in_event) {
    d.slice_at = at;
    d.used = 0;
    d.in_event = in_event;
}

/* A slice's end: the event it ran in is scheduled after it. The first
 * slice of a list _addque2 started after its wait ends before the mask's
 * restore (Psyz_GpuTimingEnd). */
static void dma_end_slice(int done) {
    uint64_t end = d.slice_at + d.used;
    if (gpu_trace_level() >= 2)
        fprintf(stderr, "DMAS 2 used %u done %d tick %llu\n", d.used, done,
                (unsigned long long)d.slice_at);
    if (d.in_event) {
        d.in_event = 0;
        gpu_schedule(end);
    }
    if (d.masked == 1) {
        d.masked = 2;
        d.first_end = end;
    }
}

/* Before a node: the DMA goes on while the FIFO is empty and the slice has
 * clocks left; else it halts or waits for the command event. */
static void dma_before_node(void) {
    if (d.len) {
        dma_end_slice(0);
        for (;;) {
            uint64_t at = d.next_run;
            gpu_fire(at);
            if (!d.len) {
                dma_start_slice(at, 1);
                return;
            }
            gpu_schedule(at);
        }
    }
    if (d.used >= DMA_SLICE_CLOCKS) {
        dma_end_slice(0);
        uint64_t end = d.slice_at + d.used;
        note_span(ns_of(end), ns_of(end + DMA_HALT_CLOCKS), 1);
        gpu_run_until(end + DMA_HALT_CLOCKS);
        dma_start_slice(end + DMA_HALT_CLOCKS, 0);
    }
}

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

/* The list DMA's interrupt that starts a queued transfer (libgpu's DMA
 * callback, GPU_QUEUE_CALLBACK_CYCLES) */
static void QueueCallback(void) {}
static int queue_nested; // waited for another exception's end
static void QueueInterrupt(void) {
    int device = 0;
    uint64_t end = Psyz_KernelExceptionEndNs(&device);
    if (end > Psyz_KernelNowNs()) {
        // during another exception libetc's dispatcher takes it in that one
        queue_nested = 1;
        Psyz_KernelPostAtNs(end, QueueInterrupt);
        return;
    }
    if (queue_nested) {
        queue_nested = 0;
        Psyz_KernelDeviceIrq(QueueCallback, 0, GPU_QUEUE_NESTED_CYCLES, 0);
        return;
    }
    unsigned chain = Psyz_CardChainInstalled() ? BIOS_CARD_CHAIN_CYCLES : 0;
    // the queued transfer's DMA, which its callback starts, stalls the CPU
    // in the exception
    unsigned total = KERNEL_IRQ_CYCLES + DMA_IRQ_DISPATCH_CYCLES + chain +
                     GPU_QUEUE_CALLBACK_CYCLES +
                     Psyz_KernelTakeStallsBeforeNs(ns_of(d.dma_free) + 1);
    Psyz_KernelDeviceIrq(QueueCallback, DEVICE_IRQ_TO_HANDLER_CYCLES,
                         total - DEVICE_IRQ_TO_HANDLER_CYCLES,
                         DMA_DISPATCH_EXIT_CLOCKS);
}

static int list_active;

void Psyz_GpuTimingBegin(void) {
    if (!timing_on()) return;
    uint64_t now = Psyz_EmulatedClocks();
    list_active = 1;
    // queued behind a transfer still in progress: its end's interrupt
    if (d.dma_free > now)
        Psyz_KernelPostAtNs(ns_of(d.dma_free), QueueInterrupt);
    int cpu_waits;
    uint64_t idle = gpu_polled_idle();
    uint64_t start = Psyz_NsToClocks(Psyz_GpuListStartNs(
        ns_of(now), ns_of(d.dma_free), ns_of(idle), &cpu_waits));
    // a queued transfer: its DMA starts in the callback of that interrupt
    if (d.dma_free > now && start < d.dma_free + GPU_QUEUE_DMA_START_CLOCKS)
        start = d.dma_free + GPU_QUEUE_DMA_START_CLOCKS;
    /* _addque2 polls for the idle GPU with the interrupts masked, then
     * starts the DMA and restores the mask after its first slice: on
     * DuckStation a CD sector's or the timer's interrupt that comes during
     * a movie frame's 43k-clock DrawOTag is taken then (Versus movie, odd
     * fields: at 93126 clocks after the blank, due from 73.6k). */
    d.masked = 0;
    if (cpu_waits) {
        start += ADDQUE2_POLL_EXIT_CLOCKS;
        d.masked = 1;
        d.first_end = start;
    }
    gpu_set_idle(idle < start ? idle : start);
    dma_start_slice(start, 0);
    list_nodes = list_words = 0;
    list_ticks = 0;
    if (gpu_trace())
        fprintf(stderr, "gpu: list at %llu dma %llu idle %llu\n",
                (unsigned long long)now, (unsigned long long)start,
                (unsigned long long)idle);
}

void Psyz_GpuTimingNode(const u_long* words, int n) {
    if (!list_active) return;
    dma_before_node();
    if (n <= 0) {
        dma_stall(DRAW_OTAG_NODE_CYCLES);
        return;
    }
    if (gpu_trace_level() >= 3) {
        fprintf(stderr, "NODE %d:", n);
        for (int k = 0; k < n && k < 16; k++)
            fprintf(stderr, " %08x", (unsigned)words[k]);
        fprintf(stderr, "\n");
    }
    dma_stall(DRAW_OTAG_NODE_CYCLES + DRAW_OTAG_BLOCK_CYCLES);
    uint32_t each[NODE_COMMANDS];
    int count = 0;
    uint64_t ticks = CommandTicks(words, n, each, &count, NODE_COMMANDS);
    gpu_push(each, count, d.slice_at + d.used);
    dma_stall((unsigned)n + ((unsigned)n + 15) / 16);
    list_nodes++;
    list_words += (unsigned)n;
    list_ticks += ticks;
}

/* Where the latest ordering table's list DMA ends, and whether it is still
 * in progress: libgpu's _addque2 then puts the next transfer in its queue
 * (sys.c DrawOTag). */
static uint64_t list_free;
int Psyz_GpuTimingTransferring(void) {
    return timing_on() && list_free > Psyz_EmulatedClocks();
}

void Psyz_GpuTimingEnd(void) {
    if (!list_active) return;
    list_active = 0;
    dma_end_slice(1);
    d.dma_free = d.slice_at + d.used;
    list_free = d.dma_free;
    if (d.masked) {
        d.masked = 0;
        Psyz_KernelWaitMaskedUntilNs(ns_of(d.first_end + ADDQUE2_RESTORE_CLOCKS));
    }
    if (gpu_trace())
        fprintf(stderr, "gpu: end packets %u words %u ticks %llu dma_end %llu idle %llu\n",
                list_nodes, list_words, (unsigned long long)list_ticks,
                (unsigned long long)d.dma_free, (unsigned long long)gpu_polled_idle());
}

void Psyz_GpuTimingImage(unsigned words) {
    if (!timing_on()) return;
    uint64_t now = Psyz_EmulatedClocks();
    uint64_t start = now > d.dma_free ? now : d.dma_free;
    uint64_t idle = gpu_polled_idle();
    if (idle > start) start = idle; // A0h waits for the GPU
    gpu_set_idle(idle < start ? idle : start);
    d.masked = 0;
    dma_start_slice(start, 0);
    unsigned clocks = words + (words + 15) / 16;
    // a block transfer in slices, the halts between them
    while (clocks) {
        unsigned take = clocks < DMA_SLICE_CLOCKS ? clocks : DMA_SLICE_CLOCKS;
        dma_stall(take);
        clocks -= take;
        if (clocks) {
            uint64_t end = d.slice_at + d.used;
            note_span(ns_of(end), ns_of(end + DMA_HALT_CLOCKS), 1);
            dma_start_slice(end + DMA_HALT_CLOCKS, 0);
        }
    }
    d.dma_free = d.slice_at + d.used;
    gpu_set_idle(d.dma_free);
}

uint64_t Psyz_GpuTimingIdleNs(void) {
    uint64_t idle = gpu_polled_idle();
    return ns_of(idle > d.dma_free ? idle : d.dma_free);
}

void Psyz_GpuTimingReset(void) {
    d.dma_free = list_free = 0;
    gpu_set_idle(0);
    d.idle_at = 0;
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
