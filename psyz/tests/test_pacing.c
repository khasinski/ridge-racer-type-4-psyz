/* Frame pacing on a fake clock (field_pacing.h): a game loop of the shape
 * of Ridge Racer Type 4's (work, a VSync(1) poll for a minimum of lines,
 * VSync(0)) against modelled displays, with the frame limiter's decisions
 * of sdl3_common.h. The game's speed must be the field clock's on every
 * display and vsync mode; the loop of 0.4 (driver VSync as the clock) is
 * run too, to show the beat it had on a 60 Hz display. */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/platform/field_pacing.h"

#define NTSC_HZ (33868800.0 * 715909 / (451584.0 * 3413 * 263))
#define PAL_HZ (33868800.0 * 709379 / (451584.0 * 3406 * 314))

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

typedef enum { LOOP_LIMITER, LOOP_DRIVER_CLOCK_04 } LoopKind;

typedef struct {
    uint64_t t;           /* fake clock, ns */
    uint64_t last_return; /* the latest VSync(0) return (VSync(1) origin) */
    uint32_t frames;      /* VSync(0) calls: the game's fields */
    double field_ns;
    PsyzSimDisplay display;
    PsyzPacing pacing;
    LoopKind kind;
    /* game frames per 1 s window */
    uint64_t window_start;
    uint32_t window_frames;
    double min_rate, max_rate;
} Sim;

static uint32_t rng = 12345;
static uint64_t Work(uint64_t lo_ns, uint64_t hi_ns) {
    rng = rng * 1103515245u + 12345u;
    return lo_ns + (uint64_t)((rng >> 8) % (uint32_t)(hi_ns - lo_ns + 1));
}

static void Advance(Sim* s, uint64_t t) {
    if (t > s->t)
        s->t = t;
    Psyz_SimDisplayAdvance(&s->display, s->t);
}

/* A present as sdl3_gpu.c makes it: synced presents acquire without
 * waiting (a full queue skips the frame), blocking ones wait for an image. */
static void Present(Sim* s, bool block) {
    Advance(s, s->t);
    if (Psyz_SimDisplayFull(&s->display)) {
        if (!block) {
            s->display.skipped++;
            return;
        }
        while (Psyz_SimDisplayFull(&s->display))
            Advance(s, Psyz_SimDisplayNextFreeNs(&s->display, s->t));
    }
    s->t += 20000; /* the present call */
    Psyz_SimDisplayPresent(&s->display, s->frames, s->t);
}

/* Psyz_VideoVSync(0): the present and the wall clock's blank. */
static void VideoVSync0(Sim* s) {
    const uint64_t entry = s->t;
    if (s->kind == LOOP_DRIVER_CLOCK_04) {
        /* 0.4: driver VSync, a blocking acquire and no wall wait */
        Present(s, true);
        return;
    }
    if (s->pacing == PSYZ_PACING_BLOCKING) {
        Present(s, true);
        const uint64_t until = Psyz_FieldWaitUntilNs(entry, s->t, s->field_ns);
        if (until)
            Advance(s, until);
        return;
    }
    /* paced: the wait, then the present at the frame's display change */
    const uint64_t until = Psyz_FieldWaitUntilNs(entry, s->t, s->field_ns);
    if (until)
        Advance(s, until);
    s->t += 50000;
    Present(s, false);
}

/* libapi's VSync(0): blanks until the one after the call. */
static void VSync0(Sim* s) {
    const uint64_t target = Psyz_FieldIndexAt(s->t, s->field_ns) + 1;
    s->frames++;
    do {
        VideoVSync0(s);
    } while (Psyz_FieldIndexAt(s->t, s->field_ns) < target);
    s->last_return = s->t;
}

static void RunGame(Sim* s, double seconds, uint64_t work_lo, uint64_t work_hi) {
    const uint64_t end = (uint64_t)(seconds * 1e9);
    s->window_start = s->t;
    s->window_frames = 0;
    s->min_rate = 1e9;
    s->max_rate = 0;
    while (s->t < end) {
        Advance(s, s->t + Work(work_lo, work_hi));
        /* do {} while (VSync(1) < 0x81): 129 lines of 64 us */
        Advance(s, s->last_return + 129 * 64000 > s->t ? s->last_return + 129 * 64000 : s->t);
        VSync0(s);
        s->window_frames++;
        if (s->t - s->window_start >= 1000000000u) {
            double rate = s->window_frames * 1e9 / (double)(s->t - s->window_start);
            if (rate < s->min_rate)
                s->min_rate = rate;
            if (rate > s->max_rate)
                s->max_rate = rate;
            s->window_start = s->t;
            s->window_frames = 0;
        }
    }
}

static void SimInit(Sim* s, double display_hz, bool vrr, double field_hz,
                    int vsync_mode, bool can_sync, LoopKind kind) {
    Sim z = {0};
    *s = z;
    s->field_ns = 1e9 / field_hz;
    s->kind = kind;
    /* the display's refreshes start at an arbitrary phase */
    Psyz_SimDisplayInit(&s->display, display_hz, vrr, 3, 3141592);
    s->pacing = Psyz_PacingChoose(vsync_mode, display_hz, field_hz, can_sync, true);
    s->display.synced = s->pacing == PSYZ_PACING_SYNCED || s->pacing == PSYZ_PACING_BLOCKING ||
                        kind == LOOP_DRIVER_CLOCK_04;
}

static void TestChoose(void) {
    const int AUTO = PSYZ_PACING_VSYNC_AUTO, ON = PSYZ_PACING_VSYNC_ON,
              OFF = PSYZ_PACING_VSYNC_OFF;
    const double hz[] = {50.0, 59.94, 60.0, 75.0, 120.0, 144.0, 165.0, 240.0};
    for (unsigned i = 0; i < sizeof(hz) / sizeof(hz[0]); i++) {
        PsyzPacing p = Psyz_PacingChoose(AUTO, hz[i], NTSC_HZ, true, true);
        CHECK(p == (hz[i] < 57.0 ? PSYZ_PACING_IMMEDIATE : PSYZ_PACING_SYNCED),
              "auto %.2f Hz: %d", hz[i], p);
        CHECK(Psyz_PacingChoose(ON, hz[i], NTSC_HZ, true, true) == PSYZ_PACING_SYNCED,
              "on %.2f Hz", hz[i]);
        CHECK(Psyz_PacingChoose(OFF, hz[i], NTSC_HZ, true, true) == PSYZ_PACING_IMMEDIATE,
              "off %.2f Hz", hz[i]);
        /* no non-blocking synced presents (OpenGL): a blocking swap only
         * about the field rate, or when asked for */
        p = Psyz_PacingChoose(AUTO, hz[i], NTSC_HZ, false, true);
        CHECK(p == (hz[i] >= 57.0 && hz[i] <= 62.9 ? PSYZ_PACING_BLOCKING
                                                   : PSYZ_PACING_IMMEDIATE),
              "auto, blocking only, %.2f Hz: %d", hz[i], p);
        CHECK(Psyz_PacingChoose(ON, hz[i], NTSC_HZ, false, true) == PSYZ_PACING_BLOCKING,
              "on, blocking only, %.2f Hz", hz[i]);
    }
    CHECK(Psyz_PacingChoose(PSYZ_PACING_VSYNC_LIMITLESS, 60.0, NTSC_HZ, true, true) ==
              PSYZ_PACING_UNPACED,
          "limitless");
    CHECK(Psyz_PacingChoose(AUTO, 0.0, NTSC_HZ, true, true) == PSYZ_PACING_SYNCED,
          "unknown refresh: as 60 Hz");
}

static void TestWaitUntil(void) {
    const double f = 1e9 / NTSC_HZ;
    /* called mid-field: the next blank */
    uint64_t b1 = Psyz_FieldBlankNs(1001, f);
    CHECK(Psyz_FieldWaitUntilNs(b1 - 9000000, b1 - 8000000, f) == b1, "next blank");
    /* a present that waited past the blank has spent the wait */
    CHECK(Psyz_FieldWaitUntilNs(b1 - 9000000, b1 + 1000, f) == 0, "spent");
    /* called just after a blank: the following one, not that one */
    CHECK(Psyz_FieldWaitUntilNs(b1 + 1000, b1 + 2000, f) == Psyz_FieldBlankNs(1002, f),
          "after a blank");
}

/* Every display and mode: the game's fields at the field clock's rate. */
static void TestSpeed(void) {
    static const struct {
        double hz;
        bool vrr;
        int mode;
        bool can_sync;
    } cases[] = {
        {60.0, false, PSYZ_PACING_VSYNC_AUTO, true},
        {59.94, false, PSYZ_PACING_VSYNC_AUTO, true},
        {60.0, false, PSYZ_PACING_VSYNC_ON, true},
        {60.0, false, PSYZ_PACING_VSYNC_OFF, true},
        {59.5, false, PSYZ_PACING_VSYNC_AUTO, true},
        {75.0, false, PSYZ_PACING_VSYNC_AUTO, true},
        {120.0, false, PSYZ_PACING_VSYNC_AUTO, true},
        {144.0, false, PSYZ_PACING_VSYNC_AUTO, true},
        {165.0, false, PSYZ_PACING_VSYNC_AUTO, true},
        {240.0, false, PSYZ_PACING_VSYNC_ON, true},
        {50.0, false, PSYZ_PACING_VSYNC_AUTO, true},
        {50.0, false, PSYZ_PACING_VSYNC_ON, true},
        {144.0, true, PSYZ_PACING_VSYNC_AUTO, true},
        {165.0, true, PSYZ_PACING_VSYNC_ON, true},
        {60.0, false, PSYZ_PACING_VSYNC_AUTO, false},  /* OpenGL */
        {59.94, false, PSYZ_PACING_VSYNC_AUTO, false}, /* OpenGL */
        {60.0, false, PSYZ_PACING_VSYNC_ON, false},    /* OpenGL */
    };
    const double fields[] = {NTSC_HZ, PAL_HZ};
    const double seconds = 120.0;
    for (unsigned fi = 0; fi < 2; fi++) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            Sim s;
            SimInit(&s, cases[i].hz, cases[i].vrr, fields[fi], cases[i].mode,
                    cases[i].can_sync, LOOP_LIMITER);
            RunGame(&s, seconds, 2000000, 12000000);
            const double expect = seconds * fields[fi];
            const char* what = cases[i].vrr ? "vrr" : "";
            CHECK(fabs(s.frames - expect) <= 2.0,
                  "%.2f Hz%s mode %d sync %d, %.3f Hz fields: %u frames, want %.0f",
                  cases[i].hz, what, cases[i].mode, cases[i].can_sync, fields[fi],
                  s.frames, expect);
            CHECK(s.min_rate > fields[fi] - 1.5 && s.max_rate < fields[fi] + 1.5,
                  "%.2f Hz%s mode %d: 1 s windows %.2f..%.2f",
                  cases[i].hz, what, cases[i].mode, s.min_rate, s.max_rate);
            /* presentation: about the field rate. A 60 Hz display shows
             * one field twice per beat (5.46 s), nothing is dropped; a
             * display slower than the fields drops the difference. */
            const double shown = (double)s.display.presents;
            if (cases[i].hz >= fields[fi] && s.pacing != PSYZ_PACING_IMMEDIATE)
                CHECK(s.display.skipped == 0, "%.2f Hz%s mode %d: %llu skipped",
                      cases[i].hz, what, cases[i].mode,
                      (unsigned long long)s.display.skipped);
            CHECK(shown >= expect * (cases[i].hz < fields[fi] ? cases[i].hz / fields[fi] - 0.02 : 0.999),
                  "%.2f Hz%s mode %d: %.0f shown of %.0f", cases[i].hz, what,
                  cases[i].mode, shown, expect);
            if (fi == 0 && cases[i].hz == 60.0 && !cases[i].vrr &&
                s.pacing == PSYZ_PACING_SYNCED) {
                const double beats = seconds * (60.0 - NTSC_HZ);
                CHECK(s.display.repeats <= beats + 2,
                      "60 Hz synced: %llu repeated refreshes for %.1f beats",
                      (unsigned long long)s.display.repeats, beats);
            }
            if (cases[i].vrr) {
                /* variable refresh: every field shown once, none repeated */
                CHECK(s.display.repeats == 0 && s.display.skipped == 0,
                      "vrr: %llu repeats %llu skipped",
                      (unsigned long long)s.display.repeats,
                      (unsigned long long)s.display.skipped);
            }
        }
    }
}

/* Long frames (a frame over a field) lose that field, as on the console,
 * and nothing more: the clock does not drift on. */
static void TestOverrun(void) {
    Sim s;
    SimInit(&s, 60.0, false, NTSC_HZ, PSYZ_PACING_VSYNC_AUTO, true, LOOP_LIMITER);
    RunGame(&s, 30.0, 18000000, 18000000); /* every frame 18 ms */
    CHECK(fabs(s.frames - 30.0 * NTSC_HZ / 2) <= 2.0, "18 ms frames: %u", s.frames);
}

/* The 0.4 loop: driver VSync's present was the clock. On a 60.000 Hz
 * display the fields beat against it: about three seconds at full speed,
 * three at half speed (the testers' report), while the display still
 * shows 60 frames a second. */
static void TestDriverClockBeat04(void) {
    Sim s;
    SimInit(&s, 60.0, false, NTSC_HZ, PSYZ_PACING_VSYNC_AUTO, true, LOOP_DRIVER_CLOCK_04);
    RunGame(&s, 60.0, 2000000, 6000000);
    CHECK(s.min_rate < 31.0 && s.max_rate > 59.5,
          "0.4 loop on 60 Hz: 1 s windows %.2f..%.2f (expected the beat)",
          s.min_rate, s.max_rate);
    CHECK(s.frames < 60.0 * NTSC_HZ * 0.85, "0.4 loop: %u frames", s.frames);
    printf("0.4 driver-VSync loop on 60 Hz: %u fields in 60 s (want %.0f), "
           "1 s windows %.1f..%.1f f/s\n",
           s.frames, 60.0 * NTSC_HZ, s.min_rate, s.max_rate);
}

int main(void) {
    TestChoose();
    TestWaitUntil();
    TestSpeed();
    TestOverrun();
    TestDriverClockBeat04();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("pacing tests passed\n");
    return 0;
}
