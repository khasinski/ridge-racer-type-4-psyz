# PSY-Z

## Running Tests

Automated tests to ensure no regressions are introduced between changes.

`make test` is the one way to run the suite on a desktop (Linux, macOS). It
builds `tests/build/sdl3-gpu-hw` and runs it the way CI does: the SDL GPU
renderer in a hidden window (`PSYZ_HEADLESS=gpu`, Linux also
`SDL_VIDEODRIVER=offscreen`), the dummy audio driver, and none of the PsyZ
clock or card variables of the calling shell (`PSYZ_AUDIO_CLOCK`,
`PSYZ_CARD_UNPLUG`, `PSYZ_CARD_UNFORMATTED`), from
`tests/`, the working directory the golden images and card directories are
relative to. Every test passes or skips there; a red run means a
regression. Other modes give other results by design: a visible window
paces presents by the monitor, `PSYZ_HEADLESS=1` draws with the CPU rasterizer and skips the tests
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

Diagnostics and clock switches, read once at their first use (changing
one later in the process has no effect):

| Variable | Effect |
| --- | --- |
| `PSYZ_AUDIO_CLOCK=video` | the offline clock: a deterministic field clock that stands still while the game works and moves only in its waits (VSync, a line-counter poll, a stream's next sector), the audio mixed up to each (headless test runs); `wall` or unset: the host's real-time clock |
| `PSYZ_CARD_UNPLUG=12`, `PSYZ_CARD_UNFORMATTED=12` | memory card slots that are empty or hold an unformatted card (once per slot, until `Psyz_CardSetSlotState`) |
| `PSYZ_CD_TRACE`, `PSYZ_PAD_TRACE`, `PSYZ_SPU_KEY_TRACE`, `PSYZ_SPU_REG_TRACE` | log drive commands and deliveries, libpad's exchanges, SPU key-ons or SPU voice register writes to stderr |
| `PSYZ_HEADLESS=1` / `gpu`, `PSYZ_HEADLESS_PACED`, `PSYZ_GPU_FETCH=0`, `PSYZ_VIDEO_GLES` | read when the renderer starts: no window (CPU rasterizer, or the SDL GPU renderer in a hidden window), paced presents, blending from copies instead of framebuffer fetch on Metal, GLES on the GL renderer |
| `PSYZ_FRAME_STATS`, `PSYZ_FRAME_LOG=file`, `PSYZ_PRESENT_PACING=0` | renderer statistics (present intervals, host work), one line per present, presents made in VSync before the blank wait |
| `PSYZ_PRESENT_LOG=file` | one line per image the swapchain took ("<ms> 0"); the log's `[Presentation FPS]` line each second counts presents that found no free image (`no_image`), `[Frame pacing] long frame` lines report frames over a field |
| `PSYZ_SIM_DISPLAY=<hz>[,vrr][,images=n]`, `PSYZ_SIM_VSYNC=auto\|on\|off`, `PSYZ_SIM_DISPLAY_LOG=file` | with `PSYZ_HEADLESS=gpu`: a modelled display (fixed or variable refresh, a swapchain of n images, 3 by default) instead of the hidden window's nothing, the vsync mode chosen for it, and a line per refresh ("<ms> <frame> <repeat>": the frame shown, by the count of blocking VSync calls) |
| `PSYZ_GPU=off` | no SDL GPU device, as on a system without a Vulkan or D3D12 driver: the CPU rasterizer draws and an SDL renderer presents (the fallback; `Psyz_VideoGetFeatures` reports no GPU enhancements) |
| `PSYZ_GPU_COMMAND_DESCRIPTOR_LIMIT=n` | submit the frame's command buffer every n descriptors (4..1024, default 256) |
| `PSYZ_BATCH_VERTICES=n`, `PSYZ_BATCH_PRIMITIVES=n` | draw GPU batches of at most n vertices or primitives (tests of smaller batches) |

There is no PS1 timing: the CD drive, the MDEC, SPU DMA and the memory card
complete as fast as the host does their work (a CD read in CdlModeRT keeps
the disc's rate, which its XA audio plays at), and native code takes no
emulated time.

### Windows

```batch
test.bat
```

Runs tests using Visual Studio as the CMake generator.
