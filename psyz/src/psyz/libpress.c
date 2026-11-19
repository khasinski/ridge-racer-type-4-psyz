#include <stdio.h>
#include <psyz.h>
#include <libpress.h>
#include <psyz/log.h>
#include <stdlib.h>
#include <string.h>
#include "../internal.h"
#include "../timing_profile.h"
#include "gpu_timing.h"
#include "../../../decomp/src/libpress/libpress_private.h"

/* Native implementation of the MDEC commands used by LIBPRESS. Serialized
 * words are always 32 bits, even where the SDK's u_long is a native pointer. */
static struct {
    DECDCTENV env;
    u8* output;
    size_t capacity, length, cursor;
    u32 command;
    int error;
    int out_pending;   // an output transfer is under way (CHCR busy)
    uint64_t out_ns;   // its interrupt's time, once its last block is out
} mdec;

/* The decoder and its output DMA unit by unit, as DuckStation runs them
 * (mdec.cpp, dma.cpp at the reference commit): a unit (a macroblock, 192
 * words in 24-bit) reaches the output FIFO MDEC_UNIT_CLOCKS after the
 * decoder starts it, and the decoder starts the next one only once the
 * FIFO is empty again. The output DMA (sync mode 1, 32-word blocks of 34
 * clocks) takes a full FIFO in slices of at most MDEC_DMA_SLICE_CLOCKS
 * with the DMA halted MDEC_DMA_HALT_CLOCKS between them (the halt is the
 * whole DMA's: a GPU transfer's slices and halts hold it too), so a
 * macroblock takes 2958 clocks: 3 blocks, the halt, 3 blocks, the next
 * decode 2688 from the read that empties the FIFO. Times are clocks on
 * the CPU's clock. A GPU DMA slice stalls the CPU, so an event due during
 * one runs at its end, its slice then shortened by the delay (one block a
 * slice). */
static struct {
    int busy;            // the decoder holds a decode
    unsigned left;       // units still to decode (from the input)
    unsigned unit_blocks; // DMA blocks a unit
    int full;            // a unit waits in the FIFO
    unsigned blocks;     // blocks of the FIFO's unit still to go out
    unsigned out_units;  // units the enabled transfer still takes
    int step;            // the pending event: 1 copy-out, 2 unhalt
    uint64_t due;        // its time
} mdp;

/* The decoder takes its PS1 time (the pipeline below), so an output
 * transfer completes, and raises its DMA callback, once the decoder has
 * produced its words and the DMA has moved them. Psyz_MdecSetInstantTiming(1)
 * completes transfers at once. */
static int mdec_instant;

int Psyz_MdecTrace(void) {
    static int trace = -1;
    if (trace < 0) {
        const char* v = getenv("PSYZ_MDEC_TRACE");
        trace = v && *v == '1';
    }
    return trace;
}

void Psyz_MdecSetInstantTiming(int instant) {
    Psyz_KernelLockInterrupts();
    mdec_instant = instant != 0;
    Psyz_KernelUnlockInterrupts();
}

static unsigned unit_bytes(void) {
    switch ((mdec.command >> 27) & 3) {
    case 0:
        return 32;
    case 1:
        return 64;
    case 2:
        return 768;
    default:
        return 512;
    }
}

/* The output DMA's interrupt. While another exception runs (the vblank's,
 * a root counter's, the CD drive's) it waits for that exception's handlers:
 * libetc's dispatcher then takes it within the same exception. */
static int out_host;
/* A root counter's exception: libetc's dispatcher looks at the interrupt
 * status at points of its work (DMA_RCNT_*_LOOK_CLOCKS, later by the output
 * DMA's own stalls), and takes the DMA's interrupt at the first after it
 * came, with its output DMA_RCNT_*_OUT_CLOCKS in; after the last, at the
 * exception's end. The port runs the callback at the exception's end either
 * way and starts its output where the console's comes (out_shift_clocks:
 * earlier, or later where the exception's end comes before it). */
static uint64_t clocks_to_ns(uint64_t clocks);
static uint64_t ns_to_clocks(uint64_t ns);
static int64_t out_shift_clocks; // clocks the output comes before the callback's
static void out_done(void) {
    int device = 0;
    uint64_t end = Psyz_KernelExceptionEndNs(&device);
    uint64_t now = Psyz_KernelNowNs();
    if (!mdec_instant && end > now) {
        int rcnt = 0;
        uint64_t start = Psyz_KernelExceptionStartNs(&rcnt);
        out_shift_clocks = 0;
        if (rcnt && !device) {
            static const unsigned look[] = {DMA_RCNT_FIRST_LOOK_CLOCKS,
                                            DMA_RCNT_SECOND_LOOK_CLOCKS,
                                            DMA_RCNT_THIRD_LOOK_CLOCKS};
            static const unsigned out[] = {DMA_RCNT_FIRST_OUT_CLOCKS,
                                           DMA_RCNT_SECOND_OUT_CLOCKS,
                                           DMA_RCNT_THIRD_OUT_CLOCKS};
            for (unsigned k = 0; k < 3; k++) {
                uint64_t at = start + clocks_to_ns(look[k]);
                at += clocks_to_ns(Psyz_KernelDmaClocksIn(start, at));
                if (now > at) {
                    continue;
                }
                // the callback at the exception's end, the output out[k] in
                int64_t late = (int64_t)ns_to_clocks(end) + DMA_AFTER_LINE_TO_HANDLER_CYCLES +
                               DMA_CALLBACK_TO_OUT_CLOCKS;
                out_shift_clocks = late - (int64_t)(ns_to_clocks(start) + out[k]);
                break;
            }
        }
        out_host = device ? 2 : 1;
        Psyz_KernelPostAtNs(end, out_done);
        return;
    }
    mdec.out_pending = 0;
    int host = out_host;
    out_host = 0;
    Psyz_DmaIrqComplete(1, host);
    out_shift_clocks = 0; // its callback output nothing
}

int Psyz_MdecOutBusy(void) {
    Psyz_KernelLockInterrupts();
    int busy = mdec.out_pending;
    Psyz_KernelUnlockInterrupts();
    return busy;
}

static void pipe_event(void);

static void cancel_out(void) {
    if (mdec.out_pending) {
        Psyz_KernelCancel(out_done);
        mdec.out_pending = 0;
        out_host = 0;
    }
    mdp.out_units = 0;
}

// the decoder stops: no unit in progress or waiting
static void pipe_reset(void) {
    Psyz_KernelCancel(pipe_event);
    mdp.busy = mdp.full = 0;
    mdp.left = mdp.blocks = mdp.out_units = 0;
    mdp.step = 0;
}

/* Clocks of a unit's decode: its blocks' (a colour macroblock is 6). */
static uint64_t unit_clocks(void) {
    unsigned blocks = unit_bytes() >= 512 ? 6 : 1;
    return (uint64_t)blocks * MDEC_BLOCK_CLOCKS;
}

static uint64_t clocks_to_ns(uint64_t clocks) { // rounded up: due by then
    return clocks / 33868800u * 1000000000u +
           (clocks % 33868800u * 1000000000u + 33868799u) / 33868800u;
}
static uint64_t ns_to_clocks(uint64_t ns) { return Psyz_NsToClocks(ns); }

static void pipe_post(int step, uint64_t due) {
    mdp.step = step;
    mdp.due = due;
    Psyz_KernelPostAtNs(clocks_to_ns(due), pipe_event);
}

// the transfer's last block is out: its interrupt
static void out_irq_at(uint64_t clocks) {
    mdec.out_ns = clocks_to_ns(clocks);
    if (Psyz_MdecTrace()) {
        fprintf(stderr, "mdec ready %llu clocks\n", (unsigned long long)clocks);
    }
    Psyz_KernelPostAtNs(mdec.out_ns, out_done);
}

// The decoder starts its next unit, the FIFO being empty from `at`.
static void pipe_decode(uint64_t at) {
    if (!mdp.left) {
        mdp.busy = 0;
        return;
    }
    mdp.left--;
    pipe_post(1, at + unit_clocks());
}

/* The output DMA takes the FIFO's unit from `due`, the time its event is
 * for, the emulator running it at `at` (later when it ran it late), as
 * dma.cpp does: a slice of MDEC_DMA_SLICE_CLOCKS less that delay (a block
 * at least), then the halt, scheduled from the event's time and the
 * transfers made in the same run of the events, which goes on to the
 * events already due then. */
static void pipe_transfer(uint64_t due, uint64_t at) {
    const unsigned block = MDEC_DMA_BLOCK_CLOCKS;
    uint64_t moved = 0; // this run's transfers (the CPU's pending clocks)
    for (;;) {
        uint64_t late = at > due ? at - due : 0;
        unsigned limit = late < MDEC_DMA_SLICE_CLOCKS
                             ? MDEC_DMA_SLICE_CLOCKS - (unsigned)late : 1;
        unsigned k = (limit + block - 1) / block;
        if (k > mdp.blocks) {
            k = mdp.blocks;
        }
        mdp.blocks -= k;
        Psyz_KernelNoteDmaSlice((at > due ? at : due) + moved, k * block);
        moved += (uint64_t)k * block;
        if (!mdp.blocks) {
            // the read of its last block empties the FIFO
            mdp.full = 0;
            if (mdp.out_units && --mdp.out_units == 0) {
                out_irq_at(at + moved);
            }
            pipe_decode(due + moved - block);
            return;
        }
        uint64_t unhalt = due + moved + MDEC_DMA_HALT_CLOCKS;
        if (unhalt > at) {
            pipe_post(2, unhalt);
            return;
        }
        due = unhalt; // already due: in the same run of the events
    }
}

/* When an event due at *due runs: at the end of a GPU DMA slice it falls
 * in (the CPU is stalled), after a GPU halt (the halt is the DMA's; the
 * MDEC's channel goes first when it ends). On DuckStation's recompiler an
 * event also waits for the end of the recompiled block being executed
 * (the kernel's register restore before an exception's rfe, about 270
 * clocks, put the second macroblock's halt 231 clocks late in 33-79% of
 * the movies' slices); the exact reference runs it at its clock
 * (tools/port/DUCKSTATION.md), as the console does. */
static uint64_t pipe_run_at(uint64_t* due) {
    uint64_t at = *due, until;
    int kind = Psyz_GpuDmaAtNs(clocks_to_ns(at), &until);
    if (kind == 2) {
        *due = at = ns_to_clocks(until);
    } else if (kind == 1) {
        at = ns_to_clocks(until);
    }
    return at;
}

static void pipe_event(void) {
    Psyz_KernelLockInterrupts();
    int step = mdp.step;
    uint64_t due = mdp.due;
    mdp.step = 0;
    if (step) {
        uint64_t at = pipe_run_at(&due);
        if (step == 1) { // a unit reaches the FIFO
            mdp.full = 1;
            mdp.blocks = mdp.unit_blocks;
        }
        if (mdp.out_units) {
            pipe_transfer(due, at);
        }
    }
    Psyz_KernelUnlockInterrupts();
}

static const u8 scan[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static int floor_shift(int64_t value, unsigned shift) {
    int64_t divisor = INT64_C(1) << shift;
    return (int)(value >= 0 ? value / divisor
                            : -((-value + divisor - 1) / divisor));
}
static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}
static int sign_bits(unsigned value, unsigned bits) {
    unsigned sign = 1u << (bits - 1);
    return (int)(value & (sign - 1)) - (int)(value & sign);
}
static u16 read16(const u8* p) { return p[0] | (u16)p[1] << 8; }
static u32 read32(const u8* p) { return read16(p) | (u32)read16(p + 2) << 16; }

static void inverse_dct(int block[64]) {
    int work[64];
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                int64_t sum = 0;
                for (unsigned z = 0; z < 8; ++z)
                    sum += (int64_t)block[y + z * 8] *
                           floor_shift(mdec.env.dct[x + z * 8], 3);
                work[x + y * 8] = floor_shift(sum + 0xfff, 13);
            }
        memcpy(block, work, sizeof(work));
    }
    for (unsigned i = 0; i < 64; ++i)
        block[i] = sign_bits((unsigned)block[i], 9);
}

/* 1 = block, 0 = padding/end of input, -1 = incomplete block. */
static int decode_block(
    const u8** source, const u8* end, int block[64], const u8 quant[64]) {
    const u8* p = *source;
    unsigned code;
    do {
        if (p == end) {
            *source = p;
            return 0;
        }
        code = read16(p);
        p += 2;
    } while (code == 0xfe00);
    memset(block, 0, 64 * sizeof(*block));
    unsigned scale = code >> 10, index = 0;
    int value = sign_bits(code, 10) * quant[0];
    for (;;) {
        if (!scale)
            value = sign_bits(code, 10) * 2;
        block[scale ? scan[index] : index] = clamp(value, -1024, 1023);
        if (index == 63)
            break;
        if (p == end) {
            *source = p;
            return -1;
        }
        code = read16(p);
        p += 2;
        index += (code >> 10) + 1;
        if (index >= 64)
            break;
        value = floor_shift(
            (int64_t)sign_bits(code, 10) * quant[index] * scale + 4, 3);
    }
    inverse_dct(block);
    *source = p;
    return 1;
}

static int reserve(size_t bytes) {
    if (bytes <= mdec.capacity - mdec.length)
        return 1;
    size_t capacity = mdec.capacity ? mdec.capacity * 2 : 4096;
    if (capacity < mdec.length + bytes)
        capacity = mdec.length + bytes;
    void* allocation = realloc(mdec.output, capacity);
    if (!allocation)
        return 0;
    mdec.output = allocation;
    mdec.capacity = capacity;
    return 1;
}

static void decode(const u8* source, const u8* end) {
    unsigned depth = (mdec.command >> 27) & 3;
    int blocks[6][64];
    for (;;) {
        int result = decode_block(
            &source, end, blocks[0], depth < 2 ? mdec.env.iq_y : mdec.env.iq_c);
        if (result == 0)
            break;
        if (result < 0) {
            mdec.error = -1;
            break;
        }
        if (depth >= 2) {
            for (unsigned b = 1; b < 6; ++b) {
                if (decode_block(&source, end, blocks[b],
                                 b == 1 ? mdec.env.iq_c : mdec.env.iq_y) != 1) {
                    mdec.error = -1;
                    return;
                }
            }
        }
        unsigned bytes = depth == 0   ? 32
                         : depth == 1 ? 64
                         : depth == 2 ? 768
                                      : 512;
        if (!reserve(bytes)) {
            mdec.error = -1;
            return;
        }
        u8* out = mdec.output + mdec.length;
        if (depth < 2) {
            for (unsigned i = 0; i < 64; ++i) {
                unsigned v = (u8)clamp(blocks[0][i], -128, 127);
                if (!(mdec.command & (1u << 26)))
                    v ^= 128;
                if (depth)
                    out[i] = (u8)v;
                else if (!(i & 1))
                    out[i / 2] = (u8)(v >> 4);
                else
                    out[i / 2] |= (u8)(v & 0xf0);
            }
        } else {
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 16; ++x) {
                    unsigned chroma = x / 2 + y / 2 * 8;
                    int cr = blocks[0][chroma], cb = blocks[1][chroma];
                    int luminance =
                        blocks[2 + x / 8 + y / 8 * 2][x % 8 + y % 8 * 8];
                    int color[3] = {
                        luminance + floor_shift(359 * cr + 128, 8),
                        luminance + floor_shift(-88 * cb - 183 * cr + 128, 8),
                        luminance + floor_shift(454 * cb + 128, 8)};
                    unsigned rgb[3];
                    for (unsigned c = 0; c < 3; ++c) {
                        rgb[c] = (u8)clamp(color[c], -128, 127);
                        if (!(mdec.command & (1u << 26)))
                            rgb[c] ^= 128;
                    }
                    unsigned pixel = x + y * 16;
                    if (depth == 2) {
                        for (unsigned c = 0; c < 3; ++c)
                            out[pixel * 3 + c] = (u8)rgb[c];
                    } else {
                        unsigned five[3];
                        for (unsigned c = 0; c < 3; ++c) {
                            unsigned value =
                                (u8)clamp(color[c], -128, 127) ^ 128;
                            five[c] = clamp((value + 4) >> 3, 0, 31);
                            if (mdec.command & (1u << 26))
                                five[c] ^= 16;
                        }
                        u16 packed =
                            (u16)(five[0] | five[1] << 5 | five[2] << 10 |
                                  ((mdec.command >> 10) & 0x8000));
                        out[pixel * 2] = (u8)packed;
                        out[pixel * 2 + 1] = (u8)(packed >> 8);
                    }
                }
        }
        mdec.length += bytes;
    }
}

static void MDEC_reset_locked(int mode) {
    if (mode != 0 && mode != 1) {
        mdec.error = -1;
        return;
    }
    cancel_out();
    pipe_reset();
    mdec.length = mdec.cursor = 0;
    mdec.command = 0;
    mdec.error = 0;
    if (!mode)
        DecDCTGetEnv(&mdec.env);
}

static void MDEC_in_locked(void* buf, int size) {
    mdec.error = 0;
    if (!buf || size < 0) {
        mdec.error = -1;
        return;
    }
    const u8* data = (const u8*)buf;
    mdec.command = read32(data);
    data += 4;
    switch (mdec.command >> 29) {
    case 1: {
        mdec.length = mdec.cursor = 0;
        unsigned words = mdec.command & 0xffff;
        if ((unsigned)size < words) {
            mdec.error = -1;
            break;
        }
        decode(data, data + words * 4);
        pipe_reset();
        if (Psyz_MdecTrace()) {
            fprintf(stderr, "mdec in %u words at %llu clocks\n", words,
                    (unsigned long long)Psyz_EmulatedClocks());
        }
        if (Psyz_KernelOfflineClock()) {
            mdp.busy = 1;
            mdp.left = (unsigned)(mdec.length / unit_bytes());
            mdp.unit_blocks = (unit_bytes() / 4 + 31) / 32;
            if (mdp.left) {
                // the decode's first unit, from the input DMA's start
                mdp.left--;
                pipe_post(1, Psyz_EmulatedClocks() + MDEC_IN_TO_FIRST_UNIT_CLOCKS);
            }
        }
        break;
    }
    case 2: {
        unsigned bytes = (mdec.command & 1) ? 128 : 64;
        if ((unsigned)size < bytes / 4) {
            mdec.error = -1;
            break;
        }
        memcpy(mdec.env.iq_y, data, 64);
        if (bytes == 128)
            memcpy(mdec.env.iq_c, data + 64, 64);
        break;
    }
    case 3:
        if (size < 32) {
            mdec.error = -1;
            break;
        }
        for (unsigned i = 0; i < 64; ++i)
            mdec.env.dct[i] = (s16)read16(data + i * 2);
        break;
    default:
        break;
    }
    if (!mdec.error)
        Psyz_DmaComplete(0);
}

static void MDEC_out_locked(void* buf, int size) {
    if (!size)
        return;
    if (!buf || size < 0 || mdec.error ||
        (size_t)size > (mdec.length - mdec.cursor) / 4) {
        mdec.error = -1;
        return;
    }
    cancel_out();
    memcpy(buf, mdec.output + mdec.cursor, (size_t)size * 4);
    mdec.cursor += (size_t)size * 4;
    unsigned units = (unsigned)(((uint64_t)size * 4 + unit_bytes() - 1) / unit_bytes());
    // the CHCR write: where the CPU has got to, a callback's handler time
    // included, less what a root counter's early look saves (out_done)
    uint64_t start = (uint64_t)((int64_t)Psyz_EmulatedClocks() - out_shift_clocks);
    out_shift_clocks = 0;
    if (Psyz_MdecTrace()) {
        fprintf(stderr, "mdec out %d words at %llu clocks, unit %s\n", size,
                (unsigned long long)start, mdp.full ? "waiting" : "decoding");
    }
    if (mdec_instant) {
        Psyz_DmaIrqComplete(1, 0);
        return;
    }
    mdec.out_pending = 1;
    if (!Psyz_KernelOfflineClock() || !mdp.busy) {
        // no PS1 clock (or no decode under way): the units' nominal time
        mdec.out_ns = Psyz_KernelNowNs() +
                      clocks_to_ns((uint64_t)units * (unit_clocks() + MDEC_UNIT_DMA_CLOCKS));
        Psyz_KernelPostAtNs(mdec.out_ns, out_done);
        return;
    }
    mdp.out_units = units;
    if (mdp.step == 1 && mdp.due <= start) {
        // in a handler the clock has not reached it yet: it came before
        Psyz_KernelCancel(pipe_event);
        mdp.step = 0;
        mdp.full = 1;
        mdp.blocks = mdp.unit_blocks;
    }
    if (mdp.full) { // a unit waits: the DMA starts at once (a new
        // transfer starts even while the DMA is halted)
        pipe_transfer(start, start);
    }
}
/* The timed completion and DMA callbacks hold the same recursive lock.
 * In particular, never cancel a transfer after another thread has already
 * completed it and its callback has submitted the next slice. */
void MDEC_reset(int mode) {
    Psyz_KernelLockInterrupts();
    MDEC_reset_locked(mode);
    Psyz_KernelUnlockInterrupts();
}
void MDEC_in(void* buf, int size) {
    Psyz_KernelLockInterrupts();
    MDEC_in_locked(buf, size);
    Psyz_KernelUnlockInterrupts();
}
void MDEC_out(void* buf, int size) {
    Psyz_KernelLockInterrupts();
    MDEC_out_locked(buf, size);
    Psyz_KernelUnlockInterrupts();
}
int MDEC_in_sync(void) {
    Psyz_KernelLockInterrupts();
    int error = mdec.error;
    Psyz_KernelUnlockInterrupts();
    return error;
}
int MDEC_out_sync(void) {
    // DecDCToutSync(0) waits for the transfer, without starving the IRQ thread.
    Psyz_KernelLockInterrupts();
    while (mdec.out_pending) {
        uint64_t now = Psyz_KernelNowNs();
        uint64_t due = mdec.out_ns;
        Psyz_KernelUnlockInterrupts();
        uint64_t now_ns = Psyz_KernelNowNs();
        int advanced = due > now && Psyz_AudioWaitNs(due - now_ns) &&
                       Psyz_KernelNowNs() != now_ns;
        Psyz_KernelLockInterrupts();
        if (advanced || !mdec.out_pending || mdec.out_ns != due)
            continue;
        if (Psyz_KernelOfflineClock() || due <= now) {
            // due, or no time can pass here (inside an interrupt)
            Psyz_KernelCancel(out_done);
            mdec.out_pending = 0;
            out_host = 0;
            mdp.out_units = 0;
            Psyz_DmaIrqComplete(1, 0);
        } else {
            Psyz_KernelUnlockInterrupts();
            Psyz_KernelPoll();
            Psyz_KernelLockInterrupts();
        }
    }
    int error = mdec.error;
    Psyz_KernelUnlockInterrupts();
    return error;
}
u_long MDEC_status(void) {
    Psyz_KernelLockInterrupts();
    u32 status = 0x0004ffffu | ((mdec.command >> 2) & 0x07800000u);
    status |= mdec.cursor == mdec.length ? 0x80000000u : 0x28000000u;
    Psyz_KernelUnlockInterrupts();
    return status;
}

int DecDCTvlc(u_long* bs, u_long* buf) {
    NOT_IMPLEMENTED;
    return -1;
}
