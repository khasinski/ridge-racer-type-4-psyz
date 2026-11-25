#ifndef PSYZ_FIELD_PACING_H
#define PSYZ_FIELD_PACING_H

/* The field clock and how frames reach the display, without SDL: the
 * decisions of the frame limiter (sdl3_common.h) and a model of a display
 * for headless runs and fake-clock tests (tests/test_pacing.c).
 *
 * The game's fields come from the wall clock at the console's field rate
 * (59.826 Hz NTSC, 49.761 Hz PAL): blank n is at n / rate seconds, and a
 * blocking VSync waits for the first blank after it was called. The display
 * never paces them. A present that waits for the display (a blocking
 * swapchain acquire or swap at about the display's own rate) only delays
 * the frame: the time it waited counts towards that blank, and the game
 * never presents twice for one blank. With driver VSync as the clock (0.4),
 * a 60.000 Hz display beat against the 59.826 Hz fields every 5.75 s: half
 * of each beat the blank fell in the game's own work, before VSync was
 * called, so every field waited two refreshes and the game ran at half
 * speed for about three seconds while the display still showed 60 frames
 * a second. */

#include <stdbool.h>
#include <math.h>
#include <stdint.h>

/* The PsyzVsyncMode values (psyz/video.h). */
enum {
    PSYZ_PACING_VSYNC_AUTO,
    PSYZ_PACING_VSYNC_ON,
    PSYZ_PACING_VSYNC_OFF,
    PSYZ_PACING_VSYNC_LIMITLESS,
};

typedef enum {
    /* Frame limiter, presents synced to the display's refresh (a VSync
     * present mode) acquired without waiting: a frame that finds every
     * image queued is not shown (no tearing, the game never waits). */
    PSYZ_PACING_SYNCED,
    /* Frame limiter, presents at once (immediate or mailbox: may tear). */
    PSYZ_PACING_IMMEDIATE,
    /* Frame limiter, presents that wait for the display (a backend without
     * non-blocking synced presents: OpenGL swap interval 1, the fallback
     * renderer). The fields still come from the wall clock. */
    PSYZ_PACING_BLOCKING,
    /* No pacing (test runs). */
    PSYZ_PACING_UNPACED,
} PsyzPacing;

/* How to present for a vsync mode on a display of display_hz (0: unknown)
 * at a field rate of field_hz. can_sync: the backend can present synced
 * without waiting (SDL GPU); can_block: it can present synced at all. */
static inline PsyzPacing Psyz_PacingChoose(int vsync_mode, double display_hz,
                                           double field_hz, bool can_sync,
                                           bool can_block) {
    if (vsync_mode == PSYZ_PACING_VSYNC_LIMITLESS)
        return PSYZ_PACING_UNPACED;
    if (vsync_mode == PSYZ_PACING_VSYNC_OFF)
        return PSYZ_PACING_IMMEDIATE;
    if (display_hz <= 0.0)
        display_hz = 60.0;
    /* AUTO: synced on a display at about the field rate or faster (57 Hz
     * and up for NTSC); a slower one shows a frame now and then for two
     * refreshes or skips one, which beats tearing on every frame only
     * close to the rate. */
    const bool near_or_faster = display_hz >= field_hz * 0.95;
    if (vsync_mode == PSYZ_PACING_VSYNC_AUTO && !near_or_faster)
        return PSYZ_PACING_IMMEDIATE;
    if (can_sync)
        return PSYZ_PACING_SYNCED;
    /* a blocking swap only near the field rate: on a much faster display
     * it is short, on a slower one it would hold every field */
    if (can_block && (vsync_mode == PSYZ_PACING_VSYNC_ON ||
                      display_hz <= field_hz * 1.05))
        return PSYZ_PACING_BLOCKING;
    return PSYZ_PACING_IMMEDIATE;
}

/* ---- the field clock ---- */

/* The blank at or before t (blank n is at n * field_ns). */
static inline uint64_t Psyz_FieldIndexAt(uint64_t t_ns, double field_ns) {
    return (uint64_t)((double)t_ns / field_ns);
}
/* (rounded up, and a nanosecond on: at that time Psyz_FieldIndexAt is
 * index, never index - 1, and a wait for it never ends a field early) */
static inline uint64_t Psyz_FieldBlankNs(uint64_t index, double field_ns) {
    return (uint64_t)ceil((double)index * field_ns) + 1;
}
/* The blank a blocking VSync called at entry_ns waits for: the first one
 * after its call. A present between the call and the wait that took past
 * it (a blocking acquire or swap) has spent the wait: then 0, no wait. */
static inline uint64_t Psyz_FieldWaitUntilNs(uint64_t entry_ns, uint64_t now_ns,
                                             double field_ns) {
    const uint64_t blank =
        Psyz_FieldBlankNs(Psyz_FieldIndexAt(entry_ns, field_ns) + 1, field_ns);
    return blank > now_ns ? blank : 0;
}

/* ---- a modelled display (PSYZ_SIM_DISPLAY, tests) ----
 *
 * A fixed refresh of hz, or variable refresh up to hz (vrr), and a
 * swapchain of `images` images: one is on screen, the others queue for
 * the next refreshes (a VSync present mode, FIFO). An unsynced present is
 * shown at once (tearing). At each refresh the oldest queued frame is
 * shown, else the one on screen again (a repeat). With variable refresh a
 * frame is shown when it is presented, at most hz times a second, and a
 * screen left alone for a 30 Hz period is refreshed again (a repeat). */
#define PSYZ_SIM_DISPLAY_QUEUE 8
typedef struct {
    double hz;
    bool vrr;
    unsigned images;
    bool synced;     /* present mode: synced (VSync) or immediate */
    uint64_t origin_ns;
    uint64_t next_refresh_ns; /* the next refresh (vrr: the earliest one) */
    uint64_t last_refresh_ns;
    unsigned queued, head;
    uint32_t queue[PSYZ_SIM_DISPLAY_QUEUE];
    uint32_t shown; /* the frame (VSync wait count) on screen */
    bool any_shown;
    uint32_t refreshed; /* the frame the latest refresh showed */
    bool refreshed_valid;
    /* presents: frames queued or shown; skipped: presents without an image */
    uint64_t refreshes, repeats, skipped, presents;
    /* each refresh: when, what (shown: the frame, repeat: same as before) */
    void (*on_refresh)(void* user, uint64_t t_ns, uint32_t frame, bool repeat);
    void* user;
} PsyzSimDisplay;

static inline double Psyz_SimRefreshNs(const PsyzSimDisplay* d) {
    return 1e9 / d->hz;
}

static inline void Psyz_SimDisplayInit(PsyzSimDisplay* d, double hz, bool vrr,
                                       unsigned images, uint64_t now_ns) {
    PsyzSimDisplay z = {0};
    *d = z;
    d->hz = hz;
    d->vrr = vrr;
    d->images = images < 2 ? 2 : images > PSYZ_SIM_DISPLAY_QUEUE ? PSYZ_SIM_DISPLAY_QUEUE : images;
    d->synced = true;
    d->origin_ns = now_ns;
    d->next_refresh_ns = now_ns + (uint64_t)Psyz_SimRefreshNs(d);
    d->last_refresh_ns = now_ns;
}

static inline void Psyz_SimRefresh(PsyzSimDisplay* d, uint64_t t_ns) {
    if (d->queued) {
        d->shown = d->queue[d->head];
        d->head = (d->head + 1) % PSYZ_SIM_DISPLAY_QUEUE;
        d->queued--;
        d->any_shown = true;
    }
    /* nothing new since the last refresh */
    const bool repeat = d->refreshed_valid && d->shown == d->refreshed;
    d->refreshed = d->shown;
    d->refreshed_valid = d->any_shown;
    d->refreshes++;
    if (repeat)
        d->repeats++;
    d->last_refresh_ns = t_ns;
    if (d->on_refresh)
        d->on_refresh(d->user, t_ns, d->shown, repeat);
}

/* The refreshes up to now. */
static inline void Psyz_SimDisplayAdvance(PsyzSimDisplay* d, uint64_t now_ns) {
    const double period = Psyz_SimRefreshNs(d);
    if (d->vrr) {
        /* a queued frame shows as soon as the panel may refresh; an idle
         * panel refreshes again after a 30 Hz period */
        for (;;) {
            if (d->queued && d->next_refresh_ns <= now_ns) {
                Psyz_SimRefresh(d, d->next_refresh_ns);
            } else if (!d->queued &&
                       d->last_refresh_ns + (uint64_t)(1e9 / 30.0) <= now_ns) {
                Psyz_SimRefresh(d, d->last_refresh_ns + (uint64_t)(1e9 / 30.0));
            } else {
                break;
            }
            d->next_refresh_ns = d->last_refresh_ns + (uint64_t)period;
        }
        return;
    }
    while (d->next_refresh_ns <= now_ns) {
        Psyz_SimRefresh(d, d->next_refresh_ns);
        const uint64_t n = (uint64_t)((double)(d->next_refresh_ns - d->origin_ns) / period + 0.5) + 1;
        d->next_refresh_ns = d->origin_ns + (uint64_t)((double)n * period + 0.5);
    }
}

/* Every image is on screen or queued: a synced present has to wait. */
static inline bool Psyz_SimDisplayFull(const PsyzSimDisplay* d) {
    return d->synced && d->queued + 1 >= d->images;
}

/* When an image frees up (the next refresh). */
static inline uint64_t Psyz_SimDisplayNextFreeNs(const PsyzSimDisplay* d,
                                                 uint64_t now_ns) {
    if (!d->vrr)
        return d->next_refresh_ns;
    return d->next_refresh_ns > now_ns ? d->next_refresh_ns : now_ns;
}

/* A present of frame (after Advance to now; a synced one needs !Full). */
static inline void Psyz_SimDisplayPresent(PsyzSimDisplay* d, uint32_t frame,
                                          uint64_t now_ns) {
    if (!d->synced) {
        d->presents++;
        /* immediate: on screen at once (torn into the scan-out) */
        d->queued = 0;
        d->shown = frame;
        d->any_shown = true;
        if (d->vrr)
            Psyz_SimRefresh(d, now_ns);
        return;
    }
    if (d->queued + 1 >= d->images) {
        d->skipped++;
        return;
    }
    d->presents++;
    d->queue[(d->head + d->queued) % PSYZ_SIM_DISPLAY_QUEUE] = frame;
    d->queued++;
    if (d->vrr) {
        /* the panel takes a frame when it comes, once a refresh period
         * has passed since the last one (the caller advanced to now
         * before the present) */
        if (d->queued && d->next_refresh_ns <= now_ns) {
            Psyz_SimRefresh(d, now_ns);
            d->next_refresh_ns = now_ns + (uint64_t)Psyz_SimRefreshNs(d);
        }
    }
}

#endif
