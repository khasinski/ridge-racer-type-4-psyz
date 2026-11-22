#include <psyz.h>
#include <psyz/log.h>
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include "../internal.h"

#define N_CHANNELS 2
#define SAMPLE_SIZE sizeof(short)

static SDL_AudioStream* sdl_stream;
static unsigned output_volume = 100;
int Psyz_AudioSetOutputVolume(unsigned percent) {
    if (percent > 100)
        return -1;
    if (sdl_stream &&
        !SDL_SetAudioStreamGain(sdl_stream, (float)percent / 100.0f))
        return -1;
    output_volume = percent;
    return 0;
}
unsigned Psyz_AudioGetOutputVolume(void) { return output_volume; }

/* Game speed: the device pulls SPU samples that much faster, keeping every
 * voice, CD stream and sequence in step with the game (higher pitch). */
static unsigned audio_speed = 1;
void Sdl3Audio_SetSpeed(unsigned speed) {
    audio_speed = speed;
    if (sdl_stream)
        SDL_SetAudioStreamFrequencyRatio(sdl_stream, (float)speed);
}

static SDL_Mutex* mutex;
static bool video_clock, video_paused;
static bool video_pal;
static void OfflineClockReset(void);

/* The capture callback runs under its own mutex, not the SPU's: it may call
 * the SPU and audio APIs, and replacing it still waits for a running call. */
static PsyzAudioCaptureCallback capture_callback;
static void* capture_userdata;
static SDL_Mutex* capture_mutex;

void Psyz_AudioSetCaptureCallback(
    PsyzAudioCaptureCallback callback, void* userdata) {
    if (!capture_mutex)
        capture_mutex = SDL_CreateMutex();
    SDL_LockMutex(capture_mutex);
    capture_callback = callback;
    capture_userdata = userdata;
    SDL_UnlockMutex(capture_mutex);
}

/* Hands mixed samples to the capture callback, after the SPU mutex is
 * released: a callback that calls back into PsyZ (Psyz_AudioLock, the SPU)
 * from the audio thread cannot deadlock against the game thread. */
static void Capture(const short* samples, int frames) {
    if (!capture_mutex)
        return;
    SDL_LockMutex(capture_mutex);
    if (capture_callback)
        capture_callback(samples, frames, capture_userdata);
    SDL_UnlockMutex(capture_mutex);
}

// Audio callback: SDL pulls audio from the SPU via this callback.
// The audio driver synchronizes the SPU based on the pulled samples.
/* Underruns: the device pulls a buffer when it has played the one before,
 * so a pull that comes later than that buffer lasted (with 5 ms of slack for
 * the device's thread) means the output ran dry meanwhile. Each is logged,
 * with the count, at most once a second. */
static unsigned audio_underruns;
static Uint64 audio_last_pull_ns, audio_last_len_ns, audio_logged_ns;

static void SDLCALL audio_callback(void* userdata, SDL_AudioStream* stream,
                                   int additional_amount, int total_amount) {
    (void)userdata;
    (void)total_amount;
    if (additional_amount <= 0)
        return;
    Uint64 now = SDL_GetTicksNS();
    if (audio_last_pull_ns && !video_paused &&
        now - audio_last_pull_ns > audio_last_len_ns * 3 / 2 + 5000000u) {
        audio_underruns++;
        if (now - audio_logged_ns >= 1000000000u) {
            audio_logged_ns = now;
            WARNF("audio underrun: %u so far (a pull %.1f ms after the last)",
                  audio_underruns, (now - audio_last_pull_ns) / 1e6);
        }
    }
    audio_last_pull_ns = now;
    audio_last_len_ns = (Uint64)additional_amount * 1000000000u /
                        (N_CHANNELS * SAMPLE_SIZE * PSYZ_SPU_SAMPLE_RATE * audio_speed);
    int num_frames = additional_amount / (N_CHANNELS * SAMPLE_SIZE);
    short buf[4096] = {0};
    while (num_frames > 0) {
        int batch = num_frames;
        if (batch > 2048)
            batch = 2048;
        SDL_LockMutex(mutex);
        Psyz_SpuPullSamples(buf, batch);
        SDL_UnlockMutex(mutex);
        Capture(buf, batch);
        SDL_PutAudioStreamData(stream, buf, batch * N_CHANNELS * SAMPLE_SIZE);
        num_frames -= batch;
    }
}

static bool is_audio_init = false;
int Psyz_AudioInit(void) {
    if (is_audio_init) {
        return 0;
    }
    Psyz_SpuInit();
    const char* clock = SDL_getenv("PSYZ_AUDIO_CLOCK");
    video_clock = clock && SDL_strcmp(clock, "video") == 0;
    if (video_clock) {
        mutex = SDL_CreateMutex();
        if (!mutex)
            return -1;
        video_paused = false;
        video_pal = false;
        OfflineClockReset();
        is_audio_init = true;
        return 0;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            ERRORF("failed to init SDL audio: %s", SDL_GetError());
            return -1;
        }
    }

    mutex = SDL_CreateMutex();
    if (!mutex) {
        ERRORF("failed to create audio mutex: %s", SDL_GetError());
        return -1;
    }

    SDL_AudioSpec spec = {
        .format = SDL_AUDIO_S16,
        .channels = N_CHANNELS,
        .freq = PSYZ_SPU_SAMPLE_RATE,
    };
    sdl_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
    if (!sdl_stream) {
        ERRORF("failed to open audio device: %s", SDL_GetError());
        SDL_DestroyMutex(mutex);
        mutex = NULL;
        return -1;
    }

    SDL_SetAudioStreamGain(sdl_stream, (float)output_volume / 100.0f);
    SDL_SetAudioStreamFrequencyRatio(sdl_stream, (float)audio_speed);
    if (!SDL_ResumeAudioStreamDevice(sdl_stream)) {
        WARNF("failed to resume audio device: %s", SDL_GetError());
    }

    is_audio_init = true;
    DEBUGF("audio initialized");
    return 0;
}

void Psyz_AudioDestroy(void) {
    if (sdl_stream) {
        SDL_DestroyAudioStream(sdl_stream);
        sdl_stream = NULL;
    }
    if (mutex) {
        SDL_DestroyMutex(mutex);
        mutex = NULL;
    }
    Psyz_AudioSetCaptureCallback(NULL, NULL);
    is_audio_init = false;
    video_clock = false;
}

void Psyz_AudioLock() {
    if (mutex) {
        SDL_LockMutex(mutex);
    }
}

void Psyz_AudioUnlock() {
    if (mutex) {
        SDL_UnlockMutex(mutex);
    }
}

void Psyz_AudioPause(void) {
    audio_last_pull_ns = 0; // no underrun across a pause
    video_paused = true;
    if (sdl_stream) {
        SDL_PauseAudioStreamDevice(sdl_stream);
    }
}

void Psyz_AudioUnpause(void) {
    audio_last_pull_ns = 0; // no underrun across a pause
    video_paused = false;
    if (sdl_stream) {
        SDL_ResumeAudioStreamDevice(sdl_stream);
    }
}

int Psyz_AudioIsPaused(void) {
    if (video_clock)
        return !is_audio_init || video_paused;
    return !sdl_stream || SDL_AudioStreamDevicePaused(sdl_stream);
}

/* SDL backend clock for offline runs (the kernel's offline clock, a
 * deterministic field clock): queries and wall time never consume samples.
 * A field lasts 3406 x 314 (PAL) or 3413 x 263 (NTSC) GPU clocks,
 * 451584/709379 or 451584/715909 system clocks each: 1069484e9 / 53203425
 * or 897619e9 / 53693175 ns (75 system clocks are 451584 / 33868800 s),
 * about 886.49 or 737.24 samples of 44100 Hz.
 *
 * The time is the kernel's (Psyz_KernelAdvanceToNs), which stops at each
 * blank (Psyz_AudioNextBlankNs, Psyz_AudioBlankDue)
 * and mixes the samples due by then (Psyz_AudioMixTo). The blanks come at
 * their exact times, ns + rem / FIELD_DIVISOR ns, not on samples. With the
 * kernel on a real-time clock (a test that selects PSYZ_AUDIO_CLOCK=video
 * after startup) this file keeps that position itself. */
#define FIELD_NS_PAL 1069484000000000ull
#define FIELD_DIVISOR_PAL 53203425u
#define FIELD_NS_NTSC 897619000000000ull
#define FIELD_DIVISOR_NTSC 53693175u
#define FIELD_LINES_PAL 314u
#define FIELD_LINES_NTSC 263u
#define FIELD_FRAMES_MAX 887 // a PAL field, rounded up

/* An exact time: ns + rem / div ns. */
typedef struct {
    uint64_t ns;
    uint64_t rem, div;
} ExactTime;

static uint64_t mix_origin_ns; // the clock's time at Psyz_AudioInit
static uint64_t mixed_frames;  // samples mixed since then
static uint64_t own_ns;        // the position when the kernel's is real time
static ExactTime last_blank, next_blank;
static bool field_open; // next_blank is set (the field after a VSync's blank
                        // starts at the first wait, with the mode of then)
static bool vsync_waiting; // VSync(0) runs the clock to its blank itself

static uint64_t ClockNowNs(void) {
    return Psyz_KernelOfflineClock() ? Psyz_KernelNowNs() : own_ns;
}

static void OfflineClockReset(void) {
    uint64_t now = ClockNowNs();
    mix_origin_ns = now;
    mixed_frames = 0;
    last_blank = (ExactTime){now, 0, 1};
    field_open = false;
    vsync_waiting = false;
}

static ExactTime ExactAdd(ExactTime t, uint64_t num, uint64_t div) {
    if (t.div != div) { // a change of standard: the phase from the ns
        t.rem = t.div > 1 ? t.rem * div / t.div : 0;
        t.div = div;
    }
    t.ns += num / div;
    t.rem += num % div;
    if (t.rem >= div) {
        t.rem -= div;
        t.ns++;
    }
    return t;
}

static void OpenField(void) {
    next_blank = video_pal ? ExactAdd(last_blank, FIELD_NS_PAL, FIELD_DIVISOR_PAL)
                           : ExactAdd(last_blank, FIELD_NS_NTSC, FIELD_DIVISOR_NTSC);
    field_open = true;
}

// Mixes frames (at most FIELD_FRAMES_MAX) under the mutex, then captures
// them outside it.
static void MixAndCapture(unsigned frames) {
    short samples[2 * FIELD_FRAMES_MAX];
    SDL_LockMutex(mutex);
    Psyz_SpuPullSamples(samples, (int)frames);
    SDL_UnlockMutex(mutex);
    Capture(samples, (int)frames);
}

/* The samples due by at_ns: sample n (from 1) is mixed once the clock
 * reaches its end, n / 44100 s after Psyz_AudioInit; an SPU write between
 * two sample times reaches the next sample. One call mixes in batches of
 * at most a field. */
void Psyz_AudioMixTo(uint64_t at_ns) {
    if (!is_audio_init || !video_clock || at_ns <= mix_origin_ns)
        return;
    uint64_t due = (at_ns - mix_origin_ns) * PSYZ_SPU_SAMPLE_RATE / 1000000000u;
    while (mixed_frames < due) {
        uint64_t batch = due - mixed_frames;
        if (batch > FIELD_FRAMES_MAX)
            batch = FIELD_FRAMES_MAX;
        MixAndCapture((unsigned)batch);
        mixed_frames += batch;
    }
}

// Runs the clock to at_ns: the kernel's, or this file's own.
static void ClockAdvanceTo(uint64_t at_ns) {
    if (Psyz_KernelOfflineClock()) {
        Psyz_KernelAdvanceToNs(at_ns);
        return;
    }
    while (at_ns > own_ns) {
        uint64_t blank = Psyz_AudioNextBlankNs();
        uint64_t to = blank < at_ns ? blank : at_ns;
        Psyz_AudioMixTo(to);
        own_ns = to;
        if (to == blank && blank != UINT64_MAX)
            Psyz_AudioBlankDue();
    }
}

static bool ClockRunning(void) {
    return is_audio_init && video_clock && !video_paused;
}

uint64_t Psyz_AudioNextBlankNs(void) {
    if (!ClockRunning() || vsync_waiting)
        return UINT64_MAX;
    if (!field_open)
        OpenField();
    return next_blank.ns;
}

/* Blanks a wait crosses happen during it (Psyz_VideoWaitBlank: counter,
 * VSync interrupt), so the field count stays on the same clock as the root
 * counters, the CD drive and the mixed audio. */
void Psyz_AudioBlankDue(void) {
    last_blank = next_blank;
    OpenField();
    Psyz_VideoWaitBlank();
}

/* The offline clock runs from the first blank, not only once the game
 * initialises sound: root counters and the CD drive use it from boot. */
static void StartOfflineClock(void) {
    if (!is_audio_init && Psyz_KernelOfflineClock())
        Psyz_AudioInit();
}

int Psyz_AudioWaitNs(uint64_t ns) {
    StartOfflineClock();
    if (!ClockRunning())
        return 0;
    ClockAdvanceTo(ClockNowNs() + ns);
    return 1;
}




/* The display mode's PAL bit changed (GP1(08h), a GPU reset): fields from
 * the next one on have that length. Offline clock only. */
void Psyz_AudioVideoModeChanged(int pal) {
    StartOfflineClock();
    if (!is_audio_init || !video_clock)
        return;
    video_pal = pal;
}

/* A blank VSync(0) waited for: the rest of the current field. */
void Sdl3Audio_VBlank(bool pal) {
    StartOfflineClock();
    if (!ClockRunning())
        return;
    if (!field_open || pal != video_pal) {
        video_pal = pal;
        OpenField();
    }
    ExactTime blank = next_blank;
    vsync_waiting = true;
    ClockAdvanceTo(blank.ns);
    vsync_waiting = false;
    last_blank = blank;
    field_open = false;
}
