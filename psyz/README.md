# PSY-Z

## Running Tests

Automated tests to ensure no regressions are introduced between changes.

`make test` is the one way to run the suite on a desktop (Linux, macOS). It
builds `tests/build/sdl3-gpu-hw` and runs it the way CI does: the SDL GPU
renderer in a hidden window (`PSYZ_HEADLESS=gpu`, Linux also
`SDL_VIDEODRIVER=offscreen`), the dummy audio driver, and none of the PsyZ
timing or card variables of the calling shell (`PSYZ_AUDIO_CLOCK`,
`PSYZ_*_TIMING`, `PSYZ_CARD_UNPLUG`, `PSYZ_CARD_UNFORMATTED`), from
`tests/`, the working directory the golden images and card directories are
relative to. Every test passes or skips there; a red run means a
regression. Other modes give other results by design: a visible window
paces presents by the monitor (the interpolation tests then present other
fields), `PSYZ_HEADLESS=1` draws with the CPU rasterizer and skips the tests
that need a GPU renderer, and `PSYZ_AUDIO_CLOCK=video` puts the kernel on
the offline clock, where time passes only as audio is mixed: the tests that
wait for real time then wait forever. The `audio_clock` tests need that
clock and skip without it; run them on their own with
`PSYZ_AUDIO_CLOCK=video PSYZ_HEADLESS=1 SDL_AUDIODRIVER=dummy
tests/build/sdl3-gpu-hw/psyz_tests --filter='audio_clock::*'` from `tests/`.
Run one suite at a time:
two runs in `tests/` share its card directories and `expected/*.actual.png`.
A GPU device that runs out of memory returns black read-backs, which shows
as dozens of image failures at once: check the run's memory (physical
footprint on macOS) before reading anything into such a run.

```bash
# run tests + GTE tests using hardware acceleration
make test

# run Windows tests using mingw32 and wine
make test-wine

# consoles and Android, emulated or on real hardware
make test-psp-emu test-psp-hw
make test-ps1-emu test-ps1-hw
make test-android
```

The tests are written with [ztest](../ztest/README.md). Pass options straight to the runner, for example
`make test RUNNER_ARGS="--filter='gpu::*'"`.
When an image comparison fails, the frame is written to `tests/expected/<name>.<target>.actual.png`.

## Environment variables

Diagnostics and timing switches, read once at their first use (changing
one later in the process has no effect):

| Variable | Effect |
| --- | --- |
| `PSYZ_AUDIO_CLOCK=video` | the offline clock: emulated time advances only as samples are mixed per field (headless runs); `wall` or unset: real time |
| `PSYZ_SPU_TIMING=instant`, `PSYZ_CD_TIMING=instant`, `PSYZ_CARD_TIMING=instant` | SPU DMA, the CD drive or the memory card complete at once instead of taking their PS1 time (`Psyz_*SetInstantTiming` overrides the last two) |
| `PSYZ_CARD_UNPLUG=12`, `PSYZ_CARD_UNFORMATTED=12` | memory card slots that are empty or hold an unformatted card (once per slot, until `Psyz_CardSetSlotState`) |
| `PSYZ_CD_TRACE`, `PSYZ_CARD_TRACE`, `PSYZ_SPU_KEY_TRACE`, `PSYZ_SPU_REG_TRACE` | log drive commands, the card driver, SPU key-ons or SPU voice register writes to stderr |
| `PSYZ_HEADLESS=1` / `gpu`, `PSYZ_HEADLESS_PACED`, `PSYZ_GPU_FETCH=0`, `PSYZ_VIDEO_GLES` | read when the renderer starts: no window (CPU rasterizer, or the SDL GPU renderer in a hidden window), paced presents, blending from copies instead of framebuffer fetch on Metal, GLES on the GL renderer |
| `PSYZ_FRAME_STATS`, `PSYZ_FRAME_LOG=file`, `PSYZ_PRESENT_PACING=0` | renderer statistics (present intervals, host work), one line per present, presents made in VSync before the blank wait |
| `PSYZ_GPU=off` | no SDL GPU device, as on a system without a Vulkan or D3D12 driver: the CPU rasterizer draws and an SDL renderer presents (the fallback; `Psyz_VideoGetFeatures` reports no GPU enhancements) |
| `PSYZ_GPU_COMMAND_DESCRIPTOR_LIMIT=n` | submit the frame's command buffer every n descriptors (4..1024, default 256) |
| `PSYZ_BATCH_VERTICES=n`, `PSYZ_BATCH_PRIMITIVES=n` | draw GPU batches of at most n vertices or primitives (tests: the interpolation midpoint must not depend on the batches) |
| `PSYZ_INTERP_STATS=1` | one `[interp-stats]` line a frame: screen area by what the midpoint did with it, batches, midpoint draws and splits, table overflows |
| `PSYZ_INTERP_HOLD=share` | hold the frame before (no midpoint) when geometry without a pose in it covers more than this share of the picture (default 0.15) |
| `PSYZ_INTERP_EXPOSE=cells` | hold when this many 4x4-pixel cells show something farther in the midpoint than in both frames (default 8; 0: no test); `PSYZ_INTERP_EXPOSE_DUMP=frame` logs the cells of one frame |
| `PSYZ_INTERP_PROBE=x,y[,first,last]` | log every primitive of frames first..last covering pixel x,y (x=-1: all) |
| `PSYZ_INTERP_DEBUG=1` / `2` | tint the midpoint's unpaired tagged primitives magenta (2: untagged ones green too) |
| `PSYZ_INTERP_GHOSTS=0`, `PSYZ_INTERP_FIELD=0`, `PSYZ_INTERP_JUNCTIONS=0`, `PSYZ_INTERP_SIZE_CULLED=0` | turn off a midpoint step: ghosts of vanished identities, moving one-pose geometry with its neighbours, T-junction snapping, drawing triangles the GPU skips for their size |

The frame interpolation and its tuned thresholds are described at the top
of `src/platform/frame_interp.h`.

The PS1 time costs PsyZ spends on the offline clock, and the DuckStation runs
they were fitted from, are in `src/timing_profile.h`.

### Windows

```batch
test.bat
```

Runs tests using Visual Studio as the CMake generator.
