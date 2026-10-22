# Performance

## Multi-resolution reconstruction cost

The launcher supports 1080p, 1440p and 4K output with Quality, Balanced,
Performance and DLAA. NGX receives its recommended input dimensions; the scene
still renders at an integer emulator scale rounded up to contain that input.
Between-scale inputs therefore pay for extra scene pixels and an input resample.
For example, 4K Performance currently renders a 1440p scene, samples it to 1080p,
and reconstructs to 4K. 1080p Quality renders 720p directly and reconstructs to
1080p. The HUD raster stays at output resolution in both cases. See
[Native DLSS](native_dlss.md) for the dimension table and validation limits.
These launch checks are not a matched performance benchmark.

## Native NVIDIA reconstruction, 2026-10-15 (before review fixes)

These measurements predate the scene/HUD shader-cache correction. They are
historical bring-up results, not performance validation of the corrected renderer.
A new matched performance comparison is still needed.

Windows, RTX 5060 Ti, Vulkan, 2560x1440 output, 120 FPS cap and 16x anisotropic
filtering. Each isolated run loaded a fresh copy of the same profile at the opening
spawn, with the car stationary. The table uses guest-frame CSV samples from seconds
55â€“70 after startup. No FH2 process was running in this set. These are one-scene
measurements, not a race benchmark or a guarantee of 120 FPS.

| Mode | Scene input | HUD | Guest FPS | Guest p99 (ms) |
| --- | --- | --- | ---: | ---: |
| Native | 2560x1440 | 2560x1440 | 53.9 | 20.0 |
| DLSS Performance | 1280x720 | 2560x1440 | 88.1 | 12.6 |
| DLAA | 2560x1440 | 2560x1440 | 50.0 | 21.7 |

The earlier proxy rendered the scene at full resolution and downsampled it before
DLSS. The native path instead uses a separate 1x scene cache, game depth/object
reprojection, and viewport jitter, and reconstructs before the native HUD draws.
These measurements use the corrected tonemapped-input flag and include the public
main changes through `cc638e7` and the NVIDIA reflection-sampling correction
(SDK patch 0047). All three runs have zero scene-transfer failures;
the NVIDIA runs also have zero feature/evaluation failures. See [Native DLSS](native_dlss.md) for scope and known issues.

## Vulkan presentation pipeline cache, 2026-10-14

SDK patch 0045 records the output format when the presenter creates its pipeline.
Previously it waited for the prior submission, destroyed the pipeline and created
it again on every paint. The unfixed 75-second run created over 3,100 host graphics
pipelines; the corrected run stopped repeatedly creating that pipeline. This fixes
frame delivery overhead, without promising a particular rendering frame rate.

## Frame statistics

Frame statistics use the same definitions as RecompiledHaloReach so the two projects
compare (`src/debug/frame_stats.cpp`, enabled with `RECOMP_PERF=1`):

```
PERF: scene= src=<guest|present> t= fps= p50= p95= p99= max= low1= stutters= hitches= ontarget= pipelines=
```

- One line per 10 s window, per scene change and at exit, for each source:
  `guest` = the game's frames (VdSwap), `present` = frames the GPU plugin handed to the
  presenter (SDK patch 0024; frames that drew with an async placeholder shader are not
  presented).
- Frame times in ms; `low1` = 1000 / mean of the slowest 1% of frame times;
  `stutters` = frames longer than 2x the window's p50; `hitches` = frames longer than
  100 ms; `ontarget` = % of frames within +-20% of the target frame time (33.3 ms, or
  16.7 ms with `frame_rate = 60`; two guest vblanks); `pipelines` = graphics pipelines
  created in the window (the plugin's "Creating graphics pipeline state" log lines,
  placeholders included).
- `RECOMP_PERF_CSV=<file>` writes every frame; `RECOMP_PERF_SCENE` names the first scene;
  `SCENE:<name>` on the input FIFO starts a new one.

## Long frames (FORZA_STALL)

Always on (`src/debug/stall_watch.cpp`, #187): when the game hands no frame to VdSwap for
200 ms, `forza.log` gets a `FORZA_STALL: no frame for <ms> ms at t=<s>` line followed by the
guest call chains of the main loop's thread and the render thread at that moment, and when
the frame comes, `FORZA_STALL: frame took <ms> ms from t=<s>`. `main loop N frames` says
whether the main loop kept running (N > 0: the render thread was held up) or was stuck too.
Nothing is logged in the first minute; at most one chain dump every 10 s (50 a session).
No frame for 10 s logs `not presenting` once and `presenting again` at the end. Loading
screens can log long frames too. A player who sees a hitch can attach these lines to an
issue: the chains are guest return addresses (look them up in Ghidra). `FORZA_STALL_TEST=1`
makes the main loop sleep 500 ms once, 90 s after the first frame, to check the lines.

## Scenarios

`tools/perf_scenario.sh SCENARIO DIR [cold|warm] [flags]` drives the game through the
input FIFO and prints the PERF lines:

| Scenario | What |
| --- | --- |
| `menu` | boot (intro video), title screen, START, main menu |
| `drive` | Single Player from a profile that has not finished the prologue: intro video, then the opening drive at full throttle |
| `freeroam` | Single Player from a profile in free roam (e.g. the user's Xenia save in a scratch `--user_data_root` with `FORZA_XUID`): loading, then 90 s of driving |

`cold` starts with an empty shader cache (`--cache_root` in the run directory); `warm` uses
the normal one.

**Machine load matters.** The test machine (Ryzen 7 7800X3D, 8 cores; Radeon RX 9060 XT,
RADV) was shared with other game instances and builds, each game
using 1.5-2.5 cores. Rules:

- Numbers for this file and for before/after comparisons come from a clean window: take
  the lock file `BENCH_LOCK` (`owner expiry-epoch purpose`, see `tools/bench_lock.sh`) for at most
  15 minutes and pause other load. Delete it when done.
- The run scripts (`tools/live_session.sh`, `tools/run_forza.sh`, `tools/build.sh`)
  refuse to start while someone else holds an unexpired lock (`tools/bench_lock.sh`).
- `tools/perf_scenario.sh` records other game processes, the load average and free memory
  at the start and end and prints `status=CONTENDED` when another game ran or the
  1-minute load average was above 2. Contended results are not baselines.
- Intro videos are skipped through the input FIFO, and each scene is only labelled once
  it is reached.

## Results

Results marked CONTENDED were taken while other games ran; they are not baselines.

| Date | Commit | Scenario | Cache | Load avg | Scene | Source | fps | p50 | p95 | p99 | max | low1 | stutters | hitches | ontarget |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-10-10 | 741ccf4 | menu | warm | CONTENDED: ~15-20, 2 Reach instances | intro video | guest | 59.9 | 16.6 | 18.7 | 20.3 | 50.5 | 35.9 | 1 | 0 | n/a (60 fps video) |
| 2026-10-10 | 741ccf4 | menu | warm | CONTENDED: ~15-20, 2 Reach instances | main menu | guest | 30.0 | 33.4 | 35.4 | 36.8 | 39.1 | 26.1 | 0 | 0 | 100 |
| 2026-10-10 | 741ccf4 | menu | warm | CONTENDED: ~15-20, 2 Reach instances | main menu | present | 30.0 | 33.3 | 48.1 | 48.9 | 49.9 | 20.1 | 0 | 0 | 54-69 |
| 2026-10-10 | 741ccf4 | freeroam (Xenia save) | warm | CONTENDED: ~23, 2 Reach instances | free roam (8 windows) | guest | 27.2-29.4 | 33.5-35.7 | 47.5-55.0 | 52.7-74.2 | 54-118 | 10.3-18.5 | 0-9 | 0-1 | 57-71 |
| 2026-10-10 | 741ccf4 | freeroam (Xenia save) | warm | CONTENDED: ~23, 2 Reach instances | free roam (8 windows) | present | 27.0-29.3 | 33.4-33.8 | 40.3-56.6 | 50.9-83.1 | 54-118 | 9.3-18.5 | 0-8 | 0-1 | 81-94 |
| 2026-10-10 | 96c43fd+ | freeroam (Xenia save), SDK XMPGetPlaybackController (10 ms sleep) | warm | 1.9-4.5, no other game (CLEAN) | free roam (10 windows) | guest | 29.9-30.0 | 33.3-33.5 | 35.0-35.7 | 35.9-51.4 | 37-62 | 17.6-27.4 | 0 | 0 | 96-100 |
| 2026-10-10 | 96c43fd+ | freeroam (Xenia save), XMPGetPlaybackController answered without the sleep | warm | 4.4-4.5, no other game (CONTENDED by load) | free roam (10 windows) | guest | 29.6-30.0 | 33.3-33.5 | 35.0-36.4 | 35.9-63.5 | 37-81 | 13.7-27.0 | 0-1 | 0 | 96-100 |
| 2026-10-10 | build before 0025 (GJK hook) | freeroam (Xenia save) | warm | 2.5, no other game | free roam | guest | 29.8 | 33.3 | 36.0 | 47.7 | 75.5 | 17.1 | 5 | 0 | 96.9 |
| 2026-10-10 | build before 0025 (GJK hook) | freeroam (Xenia save) | warm | 2.5, no other game | free roam | present | 29.6 | 33.4 | 34.5 | 65.3 | 100.7 | 14.2 | 15 | 1 | 98.2 |
| 2026-10-10 | patch 0025, no hook | freeroam (Xenia save) | warm | 4.2, no other game | free roam | guest | 29.9 | 33.3 | 35.3 | 37.8 | 66.6 | 20.8 | 0 | 0 | 99.1 |
| 2026-10-10 | patch 0025, no hook | freeroam (Xenia save) | warm | 4.2, no other game | free roam | present | 29.8 | 33.4 | 34.4 | 35.5 | 68.2 | 19.1 | 3 | 0 | 99.3 |
| 2026-10-10 | frame_rate 30 | freeroam (Xenia save) | warm | 4.0-5.8, no other game (reserved) | main menu | guest | 30.0 | 33.5 | 34.1 | 34.2 | 34.4 | 29.1 | 0 | 0 | 100 |
| 2026-10-10 | frame_rate 60 | freeroam (Xenia save) | warm | 4.0-5.8, no other game (reserved) | main menu | guest | 60.0 | 16.8 | 17.3 | 17.4 | 17.8 | 56.9 | 0 | 0 | 100 |
| 2026-10-10 | frame_rate 30 | freeroam (Xenia save) | warm | 4.0-5.8, no other game (reserved) | free roam | guest | 29.9 | 33.4 | 35.5 | 38.9 | 90.4 | 18.7 | 2 | 0 | 98.7 |
| 2026-10-10 | frame_rate 30 | freeroam (Xenia save) | warm | 4.0-5.8, no other game (reserved) | free roam | present | 29.9 | 33.4 | 34.4 | 34.9 | 84.8 | 20.1 | 2 | 0 | 99.3 |
| 2026-10-10 | frame_rate 60 | freeroam (Xenia save) | warm | 4.0-5.8, no other game (reserved) | free roam | guest | 58.9 | 16.8 | 19.5 | 24.8 | 46.8 | 32.4 | 8 | 0 | 94.8 |
| 2026-10-10 | frame_rate 60 | freeroam (Xenia save) | warm | 4.0-5.8, no other game (reserved) | free roam | present | 58.8 | 16.8 | 18.0 | 25.5 | 46.7 | 31.1 | 10 | 0 | 96.6 |
| 2026-10-10 | frame_rate 30 | race (festival race, 7 AI cars) | warm | 4.0-5.8, no other game (reserved) | race | guest | 30.0 | 33.4 | 35.8 | 37.1 | 58.0 | 24.1 | 0 | 0 | 99.4 |
| 2026-10-10 | frame_rate 60 | race (festival race, 7 AI cars) | warm | 4.0-5.8, no other game (reserved) | race | guest | 41.6 | 24.8 | 30.2 | 34.1 | 44.8 | 26.4 | 0 | 0 | 18.4 |
| 2026-10-10 | frame_rate 60 | race (festival race, 7 AI cars) | warm | 4.0-5.8, no other game (reserved) | race | present | 41.6 | 24.9 | 32.4 | 34.0 | 50.3 | 26.5 | 1 | 0 | 18.6 |
| 2026-10-11 | GPU A/B: SDK as shipped (nightly runtime) | race (festival race, 7 AI cars) | warm | under 2 at start, no other game (reserved) | race | guest | 41.4 / 42.4 | 24.8 / 24.7 | 29.4 / 30.5 | 32.8 / 33.6 | 50.1 / 53.7 | 26.6 / 26.8 | 1 / 1 | 0 | 15.8 / 23.8 |
| 2026-10-11 | GPU A/B: runtime with Reach SDK patch 0028 only | race (festival race, 7 AI cars) | warm | under 2 at start, no other game (reserved) | race | guest | 49.3 | 17.8 | 25.9 | 26.8 | 32.4 | 35.8 | 0 | 0 | 56.7 |
| 2026-10-11 | GPU A/B: plugin with Reach SDK patches 0029-0032 only | race (festival race, 7 AI cars) | warm | under 2 at start, no other game (reserved) | race | guest | 50.3 | 18.4 | 26.2 | 27.8 | 42.1 | 32.7 | 1 | 0 | 60.2 |
| 2026-10-11 | GPU A/B: Reach SDK patches 0028-0032 | race (festival race, 7 AI cars) | warm | under 2 at start, no other game (reserved) | race | guest | 59.7 / 59.7 | 16.8 / 16.8 | 17.9 / 17.9 | 20.9 / 20.7 | 33.6 / 35.7 | 42.6 / 43.2 | 1 / 1 | 0 | 98.6 / 98.6 |
| 2026-10-12 | frame_rate 60, before the frame time hook | race (festival race, 7 AI cars), 26 s | warm | 1.5, no other game (reserved) | race | guest | 59.6 | 16.8 | 18.1 | 21.6 | 28.7 | 41.5 | 0 | 0 | 97.9 |
| 2026-10-12 | frame_rate 60, before the frame time hook | race (festival race, 7 AI cars), 26 s | warm | 1.5, no other game (reserved) | race | present | 59.0 | 16.8 | 17.4 | 32.1 | 43.9 | 25.4 | 14 | 0 | 97.8 |
| 2026-10-12 | frame_rate 60 (frame time hook) | race (festival race, 7 AI cars), 26 s | warm | 3.5, no other game (reserved) | race | guest | 59.6 | 16.8 | 18.0 | 21.9 | 29.1 | 42.5 | 0 | 0 | 98.0 |
| 2026-10-12 | frame_rate 60 (frame time hook) | race (festival race, 7 AI cars), 26 s | warm | 3.5, no other game (reserved) | race | present | 58.7 | 16.8 | 17.7 | 32.7 | 44.2 | 25.4 | 15 | 0 | 96.7 |
| 2026-10-12 | frame_rate 120 (240 Hz vblank) | race (festival race, 7 AI cars), 26 s | warm | 2.7, no other game (reserved) | race | guest | 75.7 | 13.0 | 17.6 | 18.6 | 23.9 | 50.8 | 0 | 0 | 25.8 |
| 2026-10-12 | frame_rate 120 (240 Hz vblank) | race (festival race, 7 AI cars), 26 s | warm | 2.7, no other game (reserved) | race | present | 73.0 | 13.1 | 17.2 | 18.2 | 30.3 | 49.5 | 1 | 0 | 19.9 |
| 2026-10-12 | frame_rate 144 (144 Hz vblank) | race (festival race, 7 AI cars), 26 s | warm | 5.1, no other game (reserved) | race | guest | 65.2 | 14.3 | 20.8 | 21.8 | 31.8 | 42.8 | 1 | 0 | 0.0 |
| 2026-10-12 | frame_rate 144 (144 Hz vblank) | race (festival race, 7 AI cars), 26 s | warm | 5.1, no other game (reserved) | race | present | 65.3 | 14.1 | 21.0 | 21.5 | 28.2 | 44.2 | 1 | 0 | 0.1 |
| 2026-10-12 | frame_rate 60, before the frame time hook | freeroam (Xenia save), 50 s | warm | 4.6, no other game (reserved) | free roam | guest | 59.9 | 16.7 | 18.1 | 19.7 | 30.5 | 42.4 | 0 | 0 | 98.3 |
| 2026-10-12 | frame_rate 60, before the frame time hook | freeroam (Xenia save), 50 s | warm | 4.6, no other game (reserved) | free roam | present | 59.9 | 16.8 | 17.5 | 18.8 | 32.9 | 47.4 | 0 | 0 | 99.5 |
| 2026-10-12 | frame_rate 60 (frame time hook) | freeroam (Xenia save), 50 s | warm | 3.6, no other game (reserved) | free roam | guest | 59.9 | 16.7 | 17.8 | 19.3 | 31.3 | 44.7 | 0 | 0 | 99.0 |
| 2026-10-12 | frame_rate 60 (frame time hook) | freeroam (Xenia save), 50 s | warm | 3.6, no other game (reserved) | free roam | present | 59.9 | 16.8 | 17.2 | 18.0 | 32.9 | 48.7 | 0 | 0 | 99.6 |
| 2026-10-12 | frame_rate 120 (240 Hz vblank) | freeroam (Xenia save), 50 s | warm | 3.7, no other game (reserved) | free roam | guest | 115.2 | 8.4 | 11.5 | 13.1 | 27.6 | 63.7 | 15 | 0 | 91.0 |
| 2026-10-12 | frame_rate 120 (240 Hz vblank) | freeroam (Xenia save), 50 s | warm | 3.7, no other game (reserved) | free roam | present | 113.1 | 8.5 | 12.5 | 16.3 | 26.7 | 55.0 | 42 | 0 | 89.0 |
| 2026-10-12 | frame_rate 144 (144 Hz vblank) | freeroam (Xenia save), 50 s | warm | 4.6, no other game (reserved) | free roam | guest | 71.9 | 13.8 | 15.3 | 16.4 | 27.7 | 52.1 | 1 | 0 | 0.1 |
| 2026-10-12 | frame_rate 144 (144 Hz vblank) | freeroam (Xenia save), 50 s | warm | 4.6, no other game (reserved) | free roam | present | 71.9 | 13.8 | 14.8 | 15.7 | 21.8 | 57.6 | 0 | 0 | 0.0 |
| 2026-10-12 | frame_rate 144, SDK patch 0037 (288 Hz vblank) | race (festival race, 7 AI cars), 26 s | warm | 1.3, no other game (reserved) | race | guest | 70.1 | 14.2 | 18.0 | 19.0 | 23.2 | 49.8 | 0 | 0 | 2.3 |
| 2026-10-12 | frame_rate 144, SDK patch 0037 (288 Hz vblank) | race (festival race, 7 AI cars), 26 s | warm | 1.3, no other game (reserved) | race | present | 70.1 | 13.9 | 18.0 | 18.4 | 23.7 | 50.5 | 0 | 0 | 2.6 |
| 2026-10-12 | frame_rate 144, SDK patch 0037 (288 Hz vblank) | freeroam (Xenia save), 50 s | warm | 4.3, no other game (reserved) | free roam | guest | 111.5 | 9.2 | 11.2 | 12.5 | 23.1 | 67.3 | 10 | 0 | 41.4 |
| 2026-10-12 | frame_rate 144, SDK patch 0037 (288 Hz vblank) | freeroam (Xenia save), 50 s | warm | 4.3, no other game (reserved) | free roam | present | 110.6 | 9.5 | 10.8 | 13.3 | 23.7 | 65.0 | 6 | 0 | 40.8 |
| 2026-10-12 | GPU A/B: SDK as shipped (0001-0037) | race (festival race, 7 AI cars), frame_rate 144 | warm | previous run's game, none other (reserved) | race | guest | 72.8 | 13.9 | 17.7 | 18.7 | 26.6 | 50.8 | 0 | 0 | 3.5 |
| 2026-10-12 | GPU A/B: + SDK patches 0036, 0038 | race, frame_rate 144 | warm | same | race | guest | 78.8 | 13.5 | 15.9 | 17.4 | 26.7 | 53.8 | 0 | 0 | 6.8 |
| 2026-10-12 | GPU A/B: + 0036, 0038, `--clear_memory_page_state=false` | race, frame_rate 144 | warm | same | race | guest | 93.2 | 10.6 | 14.3 | 15.9 | 23.7 | 54.9 | 2 | 0 | 18.5 |
| 2026-10-12 | GPU A/B: + 0036, 0038, 0039 | race, frame_rate 144 | warm | same | race | guest | 97.2 | 10.5 | 14.0 | 15.0 | 25.4 | 62.9 | 1 | 0 | 24.1 |
| 2026-10-12 | GPU A/B: SDK as shipped | race, frame_rate 120 | warm | same | race | guest | 72.3 | 13.4 | 17.6 | 18.4 | 24.0 | 51.1 | 0 | 0 | 13.6 |
| 2026-10-12 | GPU A/B: + 0036, 0038 | race, frame_rate 120 | warm | same | race | guest | 82.4 | 12.5 | 16.8 | 17.6 | 26.9 | 51.4 | 2 | 0 | 27.1 |
| 2026-10-12 | GPU A/B: + 0036, 0038, 0039 | race, frame_rate 120 | warm | 0.4, no other game (reserved) | race | guest | 90.4 | 12.0 | 13.3 | 14.2 | 36.6 | 62.1 | 1 | 0 | 34.9 |
| 2026-10-12 | GPU A/B: SDK as shipped | race, frame_rate 60 | warm | same | race | guest | 59.8 | 16.8 | 17.7 | 20.1 | 27.4 | 44.9 | 0 | 0 | 98.6 |
| 2026-10-12 | GPU A/B: + 0036, 0038, 0039 | race, frame_rate 60 | warm | same | race | guest | 60.0 | 16.8 | 17.5 | 18.1 | 35.9 | 49.6 | 1 | 0 | 99.6 |
| 2026-10-12 | GPU A/B: SDK as shipped | freeroam (Xenia save), 50 s, frame_rate 144 | warm | same | free roam | guest | 131.4 | 7.3 | 10.5 | 11.3 | 20.5 | 79.4 | 7 | 0 | 80.0 |
| 2026-10-12 | GPU A/B: + 0036, 0038, 0039 | freeroam (Xenia save), 50 s, frame_rate 144 | warm | same | free roam | guest | 140.8 | 7.1 | 8.6 | 10.6 | 21.6 | 86.6 | 5 | 0 | 93.3 |

- 2026-10-12 rows (docs/framerate.md): one reserved window, one game at a time, back to back
  (the load average is the previous run's game), the race driven at full throttle for 26 s
  from the countdown. "Before" is the binary before `src/hooks/frame_time.cpp`. Above 60 fps
  the guest frames are the render thread's (free roam: 50 s at full throttle): the main loop
  runs at the target rate whatever the render thread makes (`FORZA_FRAMETIME_LOG`), and render frames come in whole guest
  vblanks (12.5 or 16.7 ms at 120, 13.9 or 20.8 ms at 144 with a 144 Hz vblank, multiples
  of 3.5 ms with SDK patch 0037's 288 Hz), so `ontarget` (8.3 / 6.9 ms) is low when the GPU
  emulation is the bound. The 144 Hz-vblank rows are the build before patch 0037 (frame_rate
  144 falls back on a divider of one without it).
- `frame_rate = 60` (docs/framerate.md), one reserved window, one game at a time; the load
  average is the game's own (it keeps 3-4 cores busy). Free roam: 90 s at full throttle
  from the user's Xenia save; race: GoPro Millennia Masters (signed up at that save's spawn
  point), 25 s at full throttle from the start. `ontarget` counts against 16.7 ms at 60.
  Races are bound by the GPU emulation (the render thread waits in D3D's `BlockOnFence`).
- SDK patch 0025 (dot products give QNaN on overflow) A/B in one reserved window with no
  other game running (the script still says CONTENDED: the load average was above 2). The
  run before compiled 52 pipelines in free roam and the run after 20 (the first run warmed
  the shared cache), which explains the better tail after; the patch itself costs nothing
  measurable.

Reading so far:

- XMPGetPlaybackController (music player app 0xFA, message 0x7001B): RecompiledHaloReach
  found its audio thread asking once per frame and the SDK's 10 ms sleep (on non-main
  threads) holding it to 45 fps. Forza polls it from a loop instead: about 46 calls/s
  with the sleep, about 10,500/s without it (a core spinning), and free roam holds 30 fps
  either way (the two rows above). The SDK's answer stays; `FORZA_XMP_NOSLEEP=1` answers
  at once, for frame-rate experiments above 30 fps.
- (Contended runs.) The game held its 30 fps cap in menus and close to it in free roam
  (27-29 fps) even with two Reach instances running. The 12 fps the user saw has not been
  reproduced in a measured run yet; candidates are a heavier scene, a cold shader cache,
  or more load on the machine at that moment (other games and builds).
- Free roam: one guest frame in ten takes 47-55 ms (p95), and the slowest 1% 70-120 ms.
  That is the stutter to explain first.
- Main menu presents: frames reach the presenter every 33 ms on average but p95 is 48 ms
  (the present cadence alternates around the guest's regular 33 ms frames): pacing to
  look at.

## GPU emulation speed (2026-10-11)

Races at `frame_rate = 60` were bound by the emulated GPU's command processor. Profiled with
user-space `perf_event_open` sampling (frame-pointer call chains; samply refuses at
`perf_event_paranoid = 2`, this works) and a wall-clock sampler of the command processor
thread; the details are in RecompiledHaloReach `docs/perf.md` > "GPU emulation speed", where
the fixes live as SDK patches 0028-0032:

- **0028 (runtime, `librexruntime.so`):** every write-watch fault (the game writing vertex
  data the GPU had uploaded, thousands a second) rechecked the page's protection by reading
  and parsing all of `/proc/self/maps` (~1,800 lines) while holding the global critical
  region. The command processor needs that lock for its shared-memory uploads and waited on
  it ~17% of the time. Now one `PROCMAP_QUERY` ioctl (Linux 6.11+).
- **0029-0032 (GPU plugin):** register writes as ranges (they were 30% of the command
  processor: 40 M a second, each with a register-table switch only for a debug log),
  `WAIT_REG_MEM` wakes on the guest vblank interrupt instead of 1 ms sleeps, shared-memory
  requests without the lock when the pages are valid, and unchanged fetch constants keep
  their texture bindings.

Results are the four "GPU A/B" rows above (one reserved window, same game binary, two runs
where two values are given): the race goes from 41-42 fps (p99 33 ms) to 59.7 fps (p50
16.8, p99 20.8 ms, 98.6% of frames on target). Either half alone reaches about 50 fps:
frames snap to the 120 Hz guest vblank, so the command processor had to fit both. This
project's SDK overlay ships the nightly `librexruntime.so`, so 0028 needs the overlay to
carry a patched runtime as Reach's does. Screenshots of the race menu and the race match the
unpatched build.

## GPU emulation at high refresh rates (2026-10-12)

Above 60 fps the races were bound by the emulated GPU's command processor again (its thread
busy 99.7% of the time, the render thread spinning in `BlockOnFence`). RecompiledHaloReach
`docs/perf.md` > "GPU emulation at high refresh rates" has the profile; the fixes are SDK
patches carried by both projects (`docs/sdk_patches.md`):

- **0036:** a vertex or index upload no longer ends the current render pass when no draw in it
  has read the uploaded pages; it goes before the render pass with the other uploads there
  (52,000 render pass instances and 133,000 barriers a second in the race at 144 before,
  10,600 and 27,000 after).
- **0038:** the deferred command buffer is replayed into Vulkan and submitted on a thread of its
  own (that was 11% of the command processor thread).
- **0039:** `clear_memory_page_state` off by default, as in Xenia. On (the SDK's default),
  every page the CPU had written and the GPU read was uploaded again after every frame: 74,000
  uploads (2 GB) a second in the race at 144; 1,800 off.

The "GPU A/B" rows above (two reserved windows, runs back to back with one game at a time, the
same game binary and runtime, only the executable-folder plugin differs): the race at 144 goes
from 72.8 to 97.2 fps (p99 18.7 to 15.0 ms), the race at 120 from 72.3 to 90.4 fps (p99 18.4
to 14.2 ms), free roam at 144 from 131.4 to 140.8 fps, and the
race at 60 keeps 60 fps with a shorter tail (p99 20.1 to 18.1 ms). The remaining command
processor time is spread out: descriptor updates 17%, register writes 14%, hash lookups 8%,
render target updates 7%.

Installed 2026-10-12 13:39 (`tools/build_rexglue_sdk.sh`: the overlay `~/rexglue-sdk-patched-forza`
and `forza-recomp/out/build/linux`). `--vulkan_hoist_shared_memory_uploads=false`,
`--vulkan_threaded_submission=false` and `--clear_memory_page_state=true` switch each change off.

## Resolve readback (2026-10-14)

`readback_resolve = "fast"` became the default (car thumbnails, `docs/framerate.md`). Its
cost, race scenario at 144 (base build, Xenia save), runs back to back, one game at a time;
CONTENDED: a VM ran the whole time (load 4-5), so only the differences within a pair count:

| Pair | readback_resolve | Race intro fps | Race fps (p50 / p95 / p99 ms) |
| --- | --- | --- | --- |
| 1 | none | 104.2 | 91.0 (10.6 / 14.0 / 14.9) |
| 1 | fast | 98.4 | 80.6 (13.3 / 15.0 / 16.8) |
| 2 | none | 103.1 | 88.9 (11.1 / 14.9 / 16.3) |
| 2 | fast | 95.2 | 68.8 (14.2 / 19.4 / 29.2) |

So `fast` costs 6-8% in the intro and 11-23% in the race at 144: it copies every resolve
back every frame, while the game only needs the thumbnail renders. Not measured at 30 and
60. Next: read back only while a thumbnail is being made (the hooks can set the setting at
run time: it is read at each resolve), or `some` (copy on cache miss) if it gets the
thumbnails right. `readback_resolve = "none"` in `forza.toml` restores the old speed.
Superseded: the default is `some` now (next section).

### readback_resolve cost (#27, 2026-10-14/15)

`readback_resolve` (SDK `IssueCopy_ReadbackResolvePath`): in every mode but `none`, each
resolve (EDRAM to memory) also copies its written range into a host readback buffer
(extra barriers and a shared-memory state change). `fast` then waits for nothing, maps
the *other* of two buffers (last resolve of the same address and length, so a frame
late), invalidates it and memcpy's it into guest memory on every resolve; a first-seen
(address, length) is a cache miss and waits for the GPU (`AwaitAllQueueOperationsCompletion`)
and reads the new data. `some` does the same but only copies on a cache miss (buffers
are evicted after 60 unused frames, so a car picture rendered rarely is mostly a miss).
`full` waits for the GPU on every resolve. `none` skips all of it.

Measured on the TU4 build, scratch copy of the Xenia save (pristine copy for every run),
warm shader cache, one game at a time and no other game running (the BENCH lock held;
the status line says CONTENDED only because the 1-minute load average was still 1.5-4 from
the previous run, nothing else ran). Guest fps of the race scene / free roam (p50 / p99 ms),
and the race intro:

| Scenario | none | some | fast | full on resolves of 200,000-884,736 bytes (tried) |
| --- | --- | --- | --- | --- |
| Race at 144 | 93.5 / 93.0 (10.7 / 15.2) | 93.5 / 91.2 (10.6 / 15.0) | 77.8 / 78.1 (13.5 / 17.4) | 79.2 / 62.1 (13.3 / 16.3) |
| Race intro at 144 | 102.4 / 102.5 | 102.6 / 102.3 | 96.7 / 97.9 | 82.8 / 75.7 |
| Race at 60 | 60.0 (16.8 / 18.2) | not run | 59.9 (16.8 / 18.5) | 58.1 (16.9 / 24.3) |
| Free roam at 144 | 137.3 (7.2 / 11.0) | 139.9 (7.1 / 10.7) | 108.1 (9.6 / 12.1) | 85.0 (11.0 / 15.8) |

(Two numbers = two runs.) `fast` costs 16% in the race and 21% in free roam at 144 and
nothing at 60; `some` costs nothing measurable. The extra GPU copy `some` still does per
resolve is cheap; the cost of `fast` is the invalidate and `memcpy` of every resolve
(frame-sized ones, 1-4 MB, several a frame) into guest memory, and `full` (a GPU wait per
resolve) is worse still.

Thumbnails (scratch profile, Dak's Garage, Auto Upgrade of a car whose picture was noise;
new `Thumbnail_N.xdc` decoded with `tools/xdc_decode.py`):

- `fast`: a correct BMW M1 render (91,135 bytes).
- `some`: correct renders of the BMW M1 (89,165 bytes) and, 75 s later in the same session,
  a Toyota Trueno (86,605 bytes). This is the default now.
- `none`: noise (the bug of #3).
- The picture is three kinds of resolves: the car rendered into two 640x448 targets
  (1,146,880 bytes) and composed into the 768x288 picture by two resolves (864,256 and
  806,912 bytes, overlapping inside the 884,736 byte texture, so a filter on that size
  reads back nothing), and one resolve of 262,144 bytes (an extra render target the car
  shader probably samples; without its readback the car comes out darker and the side view dim, 51 KB).
  Hence the targeted ideas in the old notes (an SDK length filter, a guest hook) were not
  needed: SDK patch 0041 (copy back only resolves in a length window) was written and
  tested, then dropped because `some` is already free.
- `some` copies a resolve back only the first time its (address, length) is seen or after
  60 frames unused, so two pictures made less than 60 frames apart could come out stale;
  the game makes one every few seconds at most.

**Photo mode (#54, 2026-10-15).** The saved photo was garbage while the screen looked right:
`some` saved noise with the shapes of an old frame (and a minimap in a corner), `fast`
(v0.1.4) saved the photographed scene in orange and green contour bands. At SAVE the game
composes the photo into EDRAM, resolves it (1280x720 `k_8_8_8_8`, 3,768,320 bytes) to
physical 0x1DAC5000 and encodes it on the CPU (JPEG, `photo/<name>/image@...`; the 416x240
`thumb` is a separate, new resolve and was always right). The same range takes a depth
resolve (copy source 4) every frame, so under the old (address, length) key the photo was
a hot key: `some` never copied it back, `fast` copied the previous resolve of the range,
the depth buffer (hence the bands). Found by filling the candidate ranges with marker colours
through `/proc/PID/mem` before taking a photo: the saved photo came out in the colour of
0x1DAC5000. SDK patch 0048 adds the copy's source (depth or color) and destination format
to the key, so the photo resolve is a first-seen key and is copied back after a GPU wait.
Checked on the TU4 build (scratch copy of the Xenia save, photo saved, then loaded from
VIEW PHOTO): correct at render scale 1 and 2. A key is first-seen whenever its range alone
was, so every copy `some` made before (car thumbnails) is still made.

Race at 144 with 0048 (same plugin, fresh save copy each run, BENCH lock held; CONTENDED
only by the load average left by the previous run):

| readback_resolve | Race intro fps | Race fps (p50 / p95 / p99 ms) |
| --- | --- | --- |
| some (0048) | 103.3 | 93.7 (10.6 / 14.1 / 15.1) |
| none | 103.1 | 94.4 (10.6 / 14.0 / 15.0) |
| some (0048) | 103.3 | 93.1 (10.7 / 14.0 / 15.0) |

Saving a photo needs the name dialog (`XamShowKeyboardUI`), an SDK ImGui window: Enter or
OK with the keyboard or mouse. With `--headless=true` (no dialog) the save fails ("File
open failed").

The Paint Shop's `Design Creator` crashed the game once (guest read of address 0x1A4, with
readback on); not looked at here.

Notes from the first try:
- Pictures are 768x288 `k_8_8_8_8` tiled textures (docs/framerate.md);
  `tools/xdc_decode.py FILE.xdc OUT.png` decodes them (Thumbnail_38/44 of the Xenia save
  are correct renders: a 3/4 view and a side view of the car; Thumbnail_9 is a different,
  parts atlas).
- The save is `sub_82546168` (TU4 `sub_825B60C0`), called only from `sub_82546460`
  (callers 82555E18, 826D5E2C, 827B7E98, 827BD678). The render before it is not found yet.
- Targeted readback must switch on before the guest issues the picture's commands (the
  GPU thread resolves asynchronously, before the CPU compresses), e.g. a guest hook
  setting an SDK flag at the render start and clearing it after the save; or an SDK filter
  on the resolve size (thumbnail texture 884,736 bytes).
- The race scenario needs the pristine Xenia save (`~/xenia_canary_windows/content`), not a
  profile the game has since changed (a garage screen came up instead of the race).

## Shader cache, cold and warm (#34, 2026-10-15)

Where the pipeline cache lives: the SDK keeps no `VkPipelineCache` blob. It stores the
guest shaders (`<cache_root>/shaders/shareable/4D5309C9.xsh`) and the descriptions of
every pipeline it made (`4D5309C9.fbo.vk.xpso`); the next start translates those shaders
and creates all stored pipelines before the game runs, and the driver's own disk cache
(Mesa's, NVIDIA's) turns that creation into a lookup. `cache_root` defaults to
`<user_data_root>/cache`: `~/.local/share/forza/cache` on Linux (play.sh and the
launcher; under Flatpak, XDG_DATA_HOME is inside `~/.var/app/<id>` and persists too),
`Documents\forza\cache` on Windows before 0b40b58 and the launcher's folder
(`%LOCALAPPDATA%\ForzaReImagined\cache`) since. A pipeline not ready in time is drawn with
a placeholder (async creation, `async_shader_compilation`) and that frame is not shown
(`vulkan_async_skip_incomplete_frames`, default on): a cold cache shows as dropped frames,
not as missing objects.

Freeroam scenario, TU4 build, scratch copy of the Xenia save. Cold: empty `--cache_root`
and empty `MESA_SHADER_CACHE_DIR` (a first start on a new machine); warm: the same two
folders on the next run. CONTENDED (the other agent's two game instances ran, load 4-10),
so only the difference counts:

| Cache | Scene | Source | fps | p50 | p95 | p99 | max | low1 | stutters | hitches | pipelines |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cold | loading into free roam | present | 28.6 | 33.4 | 35.1 | 67.2 | 500.3 | 5.5 | 12 | 6 | 485 |
| warm | loading into free roam | present | 29.8 | 33.3 | 34.5 | 50.2 | 166.3 | 12.3 | 5 | 2 | 51 |
| cold | free roam, 90 s | guest | 30.0 | 33.3 | 35.9 | 40.9 | 62.3 | 21.2 | 0 | 0 | 149 |
| cold | free roam, 90 s | present | 29.4 | 33.4 | 35.5 | 66.5 | 102.4 | 14.0 | 17 | 1 | 149 |
| warm | free roam, 90 s | guest | 30.0 | 33.3 | 35.4 | 37.7 | 55.5 | 23.6 | 0 | 0 | 14 |
| warm | free roam, 90 s | present | 29.9 | 33.4 | 34.4 | 35.7 | 67.9 | 22.5 | 2 | 0 | 14 |

The warm start created the 348 stored pipelines in 0.17 s. The game itself never slows
(guest p99 41 ms cold); what the player sees on a cold cache is the presenter dropping the
frames that used a placeholder, 17 stutters in 90 s of driving, each one or two frames, and
a few half-second holds while loading. They go away once the cache is kept, so a cache that
is not kept (a folder Windows blocks or OneDrive holds, a driver cache that is cleared) means
stutter every session. `--vulkan_async_skip_incomplete_frames=false` shows those frames
instead (objects whose pipeline is not ready are missing for a frame or two).

## Steam Deck approximation (#45)

No Deck was available. The test machine (Ryzen 7 7800X3D, 5 GHz, RX 9060 XT) was restricted to 4 cores
(`taskset -c 0-3`) and each of those cores shared with N busy loops pinned to it, so every game thread gets
1/(N+1) of a core (N=1: 50 %, 2: 33 %, 3: 25 %, 4: 20 %). This approximates a slower CPU only; the Deck's GPU
(about 1/15 of this card, shared memory bandwidth, shared power budget) is not approximated. `frame_rate = 30`,
render scale 1, `present_effect = fsr`, warm cache, no other game.

| CPU share per core | Scenario | fps | p95 | on target |
| --- | --- | --- | --- | --- |
| 100 % (4 cores) | free roam | 30.0 | 33.9 | 100 |
| 50 % | free roam | 30.0 | 36.4 | 99.6 |
| 33 % | free roam | 30.0 | 38.2 | 97.0 |
| 25 % | free roam | 29.9 | 40.8 | 85.6 |
| 25 % | race | 27.9 | 49.1 | 63 |
| 20 % | race | 22.5-24.2 (3 runs) | 56-58 | 27-38 |

The profile (`tools/cpuprof_report.py`) shows the game's main guest thread and the GPU command processor
saturating first (about 0.2 and 0.15 of a 5 GHz core in a race; the rest, audio, the XMA/WMA decoder, the
Vulkan submission thread, is each under 5 %). At 20 % per core the race runs 22-24 fps, the range reported on
the Deck. Settings tried at 20 % in the race (fps, guest): `native_2x_msaa=false` 23.0,
`readback_resolve=none` 23.2, `vulkan_threaded_submission=false` 22.9, `ignore_thread_priorities=false` 22.7
(rebuilt game), against 22.5-24.2 for the defaults: none moves the CPU-bound frame rate. Shader compilation
(warm cache), audio decode and readback are not what limits it; the main thread looked like it (but see the next section: most of its CPU time was a busy wait).

### CPU time per frame (#45, 2026-10-16)

Method: the race scenario (`tools/perf_scenario.sh race`, TU4 build, a fresh copy of the Xenia save each
run, `--present_effect=fsr`, `frame_rate = 30`, warm cache) with `PERF_CPUPROF=1` (the built-in 1 ms CPU-time
sampler, `src/debug/cpu_profile.cpp`; `tools/cpuprof_report.py` run from the build directory so `./forza`
symbolizes). CPU time per frame = a thread's samples in the race scene / the frames of the scene. Call
stacks of the command processor thread came from `eu-stack -1 -p TID` taken a few hundred times during a race
(perf is not installed and samply needs `perf_event_paranoid <= 1`). BENCH lock held, no other game; the
status lines that say CONTENDED only saw the load average of the previous run.

What the profile showed (before; ms of CPU per frame at 30 fps, 5 GHz Zen 4):

- **Render thread, 31 ms of its 33 ms frame:** 93% in the D3D library's wait for the GPU (`sub_824192A0`
  calling `sub_82AAD418` in a loop, TU4 names; disc `sub_823E91F0` / `sub_829F04A8`). The game waits there
  twice a frame for the command processor to pass a point of the ring buffer (the previous frames' swaps,
  so this is where the 30 fps cap is spent: about 14 ms a wait), polling the read pointer write-back every
  few nanoseconds. Its own work is 4-5 ms. This is the "main thread saturates" of the approximation above.
- **Command processor, 7 ms:** a flat profile (packet parsing, register writes, `UpdateBindings` and its
  descriptor updates ~15%, texture and render target lookups) except for one waste: after every draw the
  memexport readback check looked the legacy setting up by name (registry lock, two `std::string`s, a hash
  lookup, the value formatted as a string): 5-15% of the thread, nearly all of its `malloc` and `memcpy`.
- **Timer thread, 1.6-3 ms:** the runtime's timer queue polls between timers (spin, `sched_yield`, 1 ms
  sleeps) and the 1 ms timestamp timer keeps it busy.
- Not wasted: an audio thread (4.5 ms, the game's own float DSP code; `std::fma` is a libm call because the
  generated code is built without `-mfma`), the XMA/WMA decoder, Vulkan submission.

Changes: `src/hooks/gpu_wait.cpp` (each wait polls as before for 20 us, yields up to 200 us, then sleeps
50 us per poll on Linux / yields on Windows; `FORZA_GPU_WAIT=spin` keeps the game's loop,
`FORZA_GPU_WAIT_LOG=1` logs the waits), SDK patches 0049 (memexport check after the empty-range test) and
0050 (the timer queue blocks on POSIX). Before = the previous plugin and runtime with `FORZA_GPU_WAIT=spin`
(a copy of the build directory with the old plugin), after = this commit; three runs each, ms of CPU per frame:

| Race, 30 fps | fps | p95 / p99 ms | ontarget | All threads | Render thread | Command processor | Timer thread |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Before (3 runs) | 30.0 / 30.0 / 30.0 | 34.4-34.6 / 34.8-35.6 | 99.5-99.8 | 52.6-53.4 | 31.3-31.4 | 7.0-7.2 | 1.6-3.2 |
| After (3 runs) | 30.0 / 30.0 / 30.0 | 34.3 / 34.7-34.9 | 99.6-99.8 | 20.7-22.4 | 4.7 | 6.7-7.1 | 0 |
| Before, 2 cores (`taskset -c 0-1`, 2 runs) | 29.6 / 29.7 | 40.5-40.8 / 49.1-50.2 | 90.2 | 53.1-53.2 | 28.3-28.7 | 8.1-8.5 | |
| After, 2 cores (2 runs) | 29.9 / 29.9 | 37.4-37.7 / 41.6-42.1 | 97.0-97.4 | 26.7-26.9 | 5.4 | 7.6-7.7 | 0 |
| Before, 20% per core (as above, 3 runs) | 25.6 / 24.5 / 25.6 | 53.1-54.4 / 61.1-63.1 | 39-49 | 25.0-25.5 | 6.0-6.3 | 4.6-4.7 | |
| After, 20% per core (3 runs) | 25.7 / 25.2 / 25.7 | 49.3-51.1 / 54.4-57.4 | 48.5-52.8 | 18.8-19.9 | 4.1-4.2 | 4.9-5.1 | |

So the game uses 59% less CPU per frame (53 to 22 ms), the render thread 85% less, the command processor
3-8% less (the memexport lookup is gone; the rest of its profile did not change), and the timer thread
nothing. With two cores the spinning render thread had one of them to itself; now the race keeps 30 fps with
97% of frames on target instead of 90% and a p99 of 42 instead of 50 ms. The 20%-per-core approximation does
not move (25.2 to 25.5 fps, within the run-to-run spread): busy loops on every core leave no idle time to
win back, and the frame there is bound by how fast each thread gets the CPU after it wakes, not by the CPU
time the game uses. On a Deck the busy wait took one of its 8 hardware threads (and the SMT sibling's share of
that core, and power from the shared CPU/GPU budget), so that is where the gain should show; a Deck log is
still needed to tell.

The launcher now selects guarded [x86-64-v3 generated code](#cpu-target-x86-64-v3-220-2026-10-21)
on supported CPUs (#233). The narrower Linux-only `FORZA_GUEST_FMA` experiment remains off by
default for manual comparisons with `FORZA_CPU_TARGET=baseline`; see
[the BC-250 FMA validation plan](bc250-performance.md). Neither change has
a confirmed BC-250 fps result.

Other ideas left: cache Vulkan descriptor sets in
`UpdateBindings` (~15% of the command processor); a futex wake from the command processor's read pointer
write-back instead of the 50 us sleeps.

## CPU target x86-64-v3 (#220, 2026-10-21)

The SDK compiles the generated code for SSE4.1 (`-msse4.1`, `rexglue_helpers.cmake`). `-DFORZA_CPU_TARGET=x86-64-v3`
(`forza-recomp/cmake/CpuTarget.cmake`) compiles it, its precompiled header and the two facade DLLs with
`-march=x86-64-v3 -ffp-contract=off` (clang-cl: `/clang:-march=x86-64-v3 /clang:-ffp-contract=off`): AVX2, FMA,
BMI1/2, F16C, LZCNT and MOVBE, which every Intel CPU since Haswell (2013), AMD since Excavator / Zen and the Steam
Deck have. The host code keeps the baseline. The launcher passes it when the build machine's CPU and OS have all
of it (`specs.x86_64_v3`: `/proc/cpuinfo` flags on Linux, CPUID and XGETBV on Windows) and `baseline` otherwise;
`FORZA_CPU_TARGET=baseline|x86-64-v3` in the launcher's environment overrides. Adapted from the guarded FMA
experiment of #176 (`-mfma -ffp-contract=off` on Linux, early CPU check).

What changes in the generated code (TU4, 246 objects): the 7,314 calls of libm's `fma` (`ppc_fma` without
`__FMA__`: a runtime CPU test, then a call) become 11,064 inline `vfmadd`/`vfmsub` instructions; 728,000
big-endian loads and stores use `movbe` instead of a load and `bswap` (`bswap` 798,000 to 105,000); BMI2 shifts
(`shlx`, `shrx`, `rorx`) and AVX2 broadcasts and permutes appear; 6.9% fewer instructions overall.

**Same results.** `-ffp-contract=off` keeps the compiler from fusing any multiply and add of its own: only
`ppc_fma`, already fused by `std::fma`, uses FMA instructions, and both round once. #176's arithmetic check
(`tests/guest_fma` there: 4,334,912 cases, float and double, four rounding modes, all FTZ/DAZ combinations)
passes bit for bit with these flags, and a separate multiply and add still compiles to `vmulsd` + `vaddsd`. The
race itself is not reproducible from run to run, whatever the build: the throttle starts on a host timer, so after
the 26 s the cars of two runs of the same build stood 20-56 m apart (mean over the 8 cars), and runs of the two
builds 16-59 m apart. That shows no difference, and could not show a small one.

Measured: TU4, `tools/perf_scenario.sh race` and `freeroam` (copy of the Xenia save, warm shared
`--cache_root`, `--present_effect=fsr`, `frame_rate = 30`, `PERF_CPUPROF=1`), A (baseline) and B (x86-64-v3)
alternating, three runs each, BENCH lock held, every run CLEAN. CPU ms per frame = a thread's 1 ms CPU
samples in the scene / its guest frames; threads by their guest entry function (`eu-stack` at the end of the scene):
main loop `sub_825CEC88`, render `sub_8261E178`, audio `sub_83075D18` (four threads, the game's mixer and DSP, the
most `ppc_fma` calls). Mean [min-max]:

| Scene | Build | fps | p99 ms | All threads | Game code (`forza`) | libm | Main loop | Render | Audio | GPU commands |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| race | A | 30.0 | 35.0 | 22.2 [21.4-22.9] | 10.8 [10.0-11.4] | 0.39 | 2.21 [1.86-2.39] | 2.32 [1.89-2.59] | 5.03 [4.91-5.09] | 7.25 |
| race | B | 30.0 | 34.9 | 21.0 [20.5-21.9] | 9.6 [9.3-10.4] | 0.06 | 1.96 [1.75-2.38] | 2.04 [1.77-2.48] | 4.19 [4.14-4.23] | 7.46 |
| free roam | A | 30.0 | 35.1 | 35.7 [34.5-36.9] | 27.5 [27.1-28.2] | 1.17 | 2.54 [2.44-2.62] | 20.2 [20.0-20.3] | 4.04 [3.86-4.17] | 3.76 |
| free roam | B | 30.0 | 35.1 | 33.0 [29.7-34.8] | 25.4 [21.9-27.2] | 0.03 | 2.19 [2.07-2.26] | 18.3 [15.3-19.9] | 3.34 [3.27-3.46] | 4.17 |

- The audio threads use 17% less CPU in both scenes (the ranges do not overlap), the main loop 14% less in free
  roam. In the race the main loop and render threads change by 0-6% between runs of the same mode: both builds
  had runs at about 2.4 and at about 1.8 ms, the means differ by 11-12% only because two of the three B runs
  fell in the lower mode.
- The render thread spends about 20 ms of every free-roam frame whatever the scene (also with the car stuck
  against a rail) and whatever the build (B: 19.8 and 19.9, once 15.3); its hottest function is `sub_82468E50`
  (29%, float math, called from `sub_82CE8948`). In the main menu behind the "MISSING CONTENT" notice that
  thread uses a whole core. Faster code does not shorten it, so it looks like work that fills a time budget,
  not a fixed amount. Worth a look for slow CPUs: it was the render thread's idle loop, see
  [Render thread idle loop](#render-thread-idle-loop-234-2026-10-21).
- The GPU command processor (SDK plugin, not rebuilt) and the frame rate (capped at 30) do not change.

**A slower CPU.** With [the approximation above](#steam-deck-approximation-45) (4 cores, each shared with 4 busy
loops: every thread gets 20% of a core), race, two runs each, alternating: A 23.6 and 23.6 fps (p95 53.4 and 57.2
ms, on target 35 and 37%), B 24.0 and 23.6 fps (54.2 and 57.4 ms, 41 and 36%): within the noise, like every
setting #45 tried there (the frame waits for each thread to get the CPU back, not for its CPU time). B used 18.5
and 19.2 ms of CPU per frame against 19.7 and 19.8, the audio threads 3.9 against 4.9-5.1. A Deck is a Zen 2 at
2.4-3.5 GHz within a 15 W budget shared with the GPU, roughly 2 to 2.5 times this machine's time per instruction:
the same 8-11% less work in the game's code (17% in the audio threads, 14% in the free-roam main loop) means more
idle time on its 8 hardware threads and less power drawn from that budget, which helps where the CPU is busy
(free roam at 60 fps) more than in this approximation. The x86-64-v3 code uses no PDEP/PEXT (slow microcode on Zen
2). A Deck log is still needed to tell the fps.

The build with the CMake option (generated code only, host at the baseline, guard included) ran the race normally:
21.4 ms of CPU per frame, game code 10.2, audio 4.1, libm 0.06.

**Guard.** A built game can be copied to an older PC, where the first AVX2 instruction would end it with SIGILL at
a random point. `src/platform/cpu_target.cpp` (compiled with the baseline flags) checks CPUID (leaf 1, leaf 7,
0x80000001) and XCR0 for every x86-64-v2 and v3 feature before any generated code runs: on Linux from
`.preinit_array`, which runs even before the shared libraries' initializers (the SDK runtime's static constructors
can call inline functions the executable exports, and those may come from a generated-code object); on Windows from
a constructor of priority 101 (`.CRT$XCA00101`, before every C++ initializer; a DLL never calls into the
executable). On a CPU without them it prints which features are missing and "Rebuild the game on this PC: in the
launcher, Setup > Build" (a message box too on Windows when not started by the launcher) and exits with code 76,
which the launcher explains instead of offering a crash report. The launcher also refuses Play itself for its own
v3 build on such a CPU, and records the target in `build/state.json` (`cpu_target`) and in the build id, so a
changed CPU (or a data folder moved to another PC) shows "Rebuild needed". Tests: `launcher/test_cpu_target.py`,
`forza-recomp/tests/cpu_target_test.cpp` (feature bits; `-DFORZA_CPU_TARGET_V3=1` also runs the guard before
`main`). With gdb clearing BMI1/2, AVX2 and MOVBE in the CPUID words, the TU4 game's guard printed them from
`_dl_init` and exited with 76; under Wine (GE-Proton 9-27), the guard cross-compiled for Windows with AVX2 hidden
did the same before the C++ initializers and `main`. The launcher's Windows CPUID machine code, run on Linux behind
an argument shim, read the same words as the game.

The TU4 game also builds for Windows with the option (cross clang-cl with xwin, `tools/cmake/windows-clang-cl.cmake`):
the generated objects have the FMA and MOVBE instructions and no `fma` calls, the guard's object none of them.
Not verified: the Windows game running (no Windows PC with a GPU here; measured on Linux only) and a Steam Deck.

## Render thread idle loop (#234, 2026-10-21)

In free roam the render thread used about 20 ms of CPU in every 33 ms frame (a race: about 2), and behind the
main menu's "MISSING CONTENT" notice a whole core. Its hottest function, `sub_82468E50` (disc `sub_82438EA8`), is
the CRT's `pow`. Both were the thread's loop waiting for the next main frame without ever blocking.

**Call chains** (perf `--call-graph dwarf` on that thread, TU4 names):

- free roam: `sub_8261E178` (the thread) > `sub_826164D0` (one pass of its loop) > `sub_825FCD50` (the screen
  gamma transition) > `sub_825FCC28` > (vtable +200) `sub_82CE8948` (builds a 128-entry PWL gamma ramp, three
  channels, and sets it with `sub_82AAEE68`; 256 entries and `sub_82AAED98` in its other mode) > `pow` 128 times,
  and `sub_82CE8810` > `sub_82CE78E0` > `pow` 128 more. `pow` and its helpers (`sub_824989D0` ldexp,
  `sub_8241F8C0` frexp, `sub_82B43BC0`) were 70% of the thread's samples, the ramp's D3D calls 10%.
- main menu: the same pass, no `pow`: its loop of 32 virtual calls on render object +0x970 (`sub_82440A20`,
  `sub_82472748`, 37%), the render-thread jobs (`sub_82F0BAB8`, 11%), the end-of-pass virtual call
  (`sub_82D13A50` > `sub_824797A8`, 16%), `sub_824AA568`'s `NtSetEvent` (8%). No kernel wait anywhere: the
  thread never blocked.

**What the loop does.** `sub_8261E178` calls `sub_826164D0` until the game ends. Each pass begins with per-pass
work (the gamma transition among it) and takes the next main frame, if there is one, to draw it (or skip it).
A pass that has none returns, in one of two ways:

- idle: the main frames' count (render object +0xA50) is not past the frame it took last (+0x94C), or, in mode 2
  (+0xCB0: free roam and races), the queue has none ready. It gives the render-thread jobs their time budget
  (`sub_82F0CD48`) and calls `SleepEx(0, TRUE)` (`sub_82B2A848`). On the console that yields to the other
  threads of the core; here it returns at once (the SDK's `AlertableSleep(0)` neither sleeps nor yields on POSIX).
- empty: the count says there is a frame but the queue has none to take yet (`sub_824A4070`: its list +0x20 and
  current +0x38 are empty). In the menus (mode 0) the count runs ahead of the queue, so every pass between two
  frames is one of these, and nothing sleeps at all.

So between two main frames the thread runs pass after pass: a busy wait for the main loop, by design of the game,
that costs a core here. The gamma transition (`sub_825FCD50`, disc `sub_82586CD8`) makes each pass dear: while
its start time (+0x16E0) is above 0 it lerps the three gamma parameters over its duration (+0x16E8, 2 s) and
rebuilds and sets the ramp. It is meant to stop once the progress passes 1, but the progress is clamped to 1
first, so a transition never ends: after the game's first gamma change (behind the menus, before free roam) every
pass rebuilds the ramp. In free roam that made about 19,000 idle passes a second (640 a frame) of about 30 us
each, nearly all of it the ramp. Behind the DLC notice no transition has started yet, so the passes are cheaper
and the thread is all loop (its `NtSetEvent`s kept other threads busy too). Faster code does not change it
(#220): faster passes only make more of them.

**Free roam against the race.** In a race the render thread spends its frame in D3D's wait for the GPU (the
previous frames' swaps, `gpu_wait.cpp` above), so the next main frame is waiting when it returns: in the race scene
of the four A runs below it never went idle (two of the B runs idled about 250 times a second). In free roam it
finishes its frame early and spends about half of each frame waiting for the next main frame.

**Change** (`src/hooks/render_idle.cpp`): such a pass, at the idle pass's `SleepEx` call and at the empty pass's
end (its call of `sub_82E9C3B8` with no frame taken; each call site told by its return address, the other six
`SleepEx` callers left alone), waits for the pass's own conditions first. As in `gpu_wait.cpp`, the first 20 us
since the thread last took a frame keep the game's loop and the next 180 us yield; then it sleeps in 50 us steps
(timer slack 5 us) until the pass would find work: a frame to take (the pass's own test), a message (+0x8) or a
render-thread job queued, or 1 ms has passed, and the pass runs again. A pass that finds jobs queued goes on at
once, as before. A new main frame is taken within about 50 us, so frame pacing does not change; the per-pass
work, the gamma ramp included, still runs, only in fewer passes (free roam: 730 a second instead of 19,000). Windows
keeps the game's loop (its `SleepEx(0)` yields; its sleeps are whole milliseconds). `FORZA_RENDER_IDLE=spin`
keeps the game's loop; `FORZA_RENDER_IDLE_LOG=1` logs the idle and empty passes, waits and time waited every 10 s.

Measured: TU4, `tools/perf_scenario.sh` (copy of the Xenia save, warm shared `--cache_root`,
`--present_effect=fsr`, `frame_rate = 30`, `PERF_CPUPROF=1`), one build, A = `FORZA_RENDER_IDLE=spin` (the game's
loop) and B = the wait, alternating, BENCH lock held. CPU ms per frame = a thread's 1 ms CPU samples in the scene /
its guest frames (as in [CPU target x86-64-v3](#cpu-target-x86-64-v3-220-2026-10-21)). Mean [min-max]:

| Scene | Mode | fps | p99 ms | All threads | Main loop | Render | Audio | GPU commands |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| free roam (3 runs each) | A | 30.0 | 35.1 [35.0-35.3] | 36.6 [36.2-36.9] | 2.50 [2.46-2.56] | 20.3 [20.2-20.4] | 4.33 [4.16-4.63] | 4.15 [4.14-4.15] |
| free roam | B | 30.0 | 35.0 [34.8-35.2] | 18.3 [16.6-19.5] | 2.27 [2.20-2.35] | 2.77 [2.71-2.89] | 4.06 [3.82-4.18] | 4.22 [3.75-4.55] |
| race (4 and 3 runs) | A | 30.0 | 34.9 [34.8-35.1] | 23.2 [22.8-23.9] | 2.27 [1.85-2.46] | 2.44 [2.30-2.60] | 5.36 [5.08-5.73] | 7.31 [7.10-7.50] |
| race | B | 30.0 | 34.8 [34.7-34.9] | 21.9 [20.6-22.6] | 2.24 [1.94-2.40] | 2.42 [2.36-2.51] | 5.00 [4.78-5.12] | 7.01 [6.49-7.28] |
| main menu, DLC notice (2 runs each) | A | 29.8 | 34.1 [34.0-34.2] | 55.6 [55.6-55.6] | 0.26 | 28.5 [28.4-28.5] | 0.33 | 0.81 |
| main menu, DLC notice | B | 30.0 | 33.8 [33.8-33.9] | 6.5 [6.4-6.5] | 0.30 | 0.41 [0.41-0.42] | 0.26 | 0.71 |

- Free roam: the render thread uses 86% less CPU (20.3 to 2.8 ms a frame), the whole game half (36.6 to 18.3).
  Idle passes went from about 19,100 to 730 a second; the thread waits 54% of the time, and about 30 waits a
  second (one a frame) end with a main frame, the rest after 1 ms.
- Main menu: the render thread from a whole core to 0.4 ms a frame and the game from 55.6 to 6.5 ms (1.7 cores to
  0.2): the spinning pass also kept other threads busy. 2,700 empty passes a second remain.
- Races do not change (the render thread hardly idles in them). A fourth B race, left out above, ran while
  another job loaded the machine (load average 7 at its end): 24.3 ms, p99 37.5.
- Frame pacing: the same frame rate, p99 and share of frames on target in every scene; the race intro (a
  cutscene) 30.0 fps, p99 35.0 ms with B; screenshots of the menu, free roam, the intro and the race looked as
  before. Free roam at 60 fps (one run each): 60.0 and 60.0 fps, p99 18.5 and 18.1 ms, render thread 3.7 and
  2.2 ms. At 240 (three runs each, bound by the GPU emulation, render sync waiting every frame): A 154-160 fps,
  B 149-172 (means 157.6 and 158.1; the drive differs between runs, 127-198 fps in 10 s windows), render thread
  3.0-3.2 against 2.1-2.2 ms.

Left: the gamma ramp (still about 30 us a pass, 730 idle passes a second in free roam: about 0.7 ms a frame)
could be cached from its inputs, or the transition ended at progress 1 (the game's intent, but every pass
re-applying the ramp is what it does now); Windows could wait on a high-resolution waitable timer instead of
yielding; with #240 the wait should also end when the main loop waits for render sync (that wait now ends up to
1 ms later). The first two are done in #259 (below).

## Gamma transition end and the Windows idle wait (#259, 2026-10-22)

**The gamma transition.** The game starts one with `sub_825FCCA0` (disc `sub_82586C28`): it sets one gamma
channel's target and the start time (render object +0x16E0). In the free roam scenario there is one: when the DLC
notice is dismissed, channel 1 goes from 0.5 to 0.7 in 2 s. Each render-thread pass then lerps the three channels
by min(progress, 1), builds and sets the ramp (`sub_825FCC28` > vtable +200 `sub_82CE8948`), and clears the start
time only when that clamped progress is above 1, so it never does. From then on every pass built the same ramp
again: 730 times a second in free roam with #234's wait, and on Windows, where the thread still spun, in every
pass.

**Change** (`src/hooks/gamma_transition.cpp`): the transition's call of `sub_825FCC28` (told by its return
address; the device setup and immediate gamma changes call it too) clears the start time after the ramp is
built at progress 1, as the game does above 1. The next gamma change starts a new transition as before.
`FORZA_GAMMA_TRANSITION=game` keeps the endless transition.

**Same ramp.** The D3D setter (`sub_82AAEE68`) keeps the applied ramp (1536 bytes at device +15408) and sends
it to the GPU only when it changes. A temporary diagnostic build (TU4: title, DLC notice, menu, free roam, race
sign-up, intro and start) hashed it after every set:

- Game's transition: 1,906 different ramps during the 2 s, then d7d40275a1988239, set again in every pass
  (760 a second) until the end of the run.
- Change: the same fade (1,909 different ramps), the transition ends 2.0 s after it starts at
  d7d40275a1988239, and nothing sets the ramp after that.
- Every 0.5 s with no transition running, the diagnostic built the ramp the endless transition would have set
  (the three channels at progress 1 from their current values). All 200 builds, through menu, free roam and the
  race, matched the applied ramp.
- Nothing changed the other inputs (+0x16E8 to +0x1713, the device's format, the flag at 0x8353A1E0) without
  starting a transition. In the code, the only writers of those fields are the constructor (`sub_8261E320`),
  `sub_825FCCA0` (which always restarts the transition) and the transition itself.
- Screenshots of every step look the same; still scenes have the same mean colour to 0.1 (sign-up
  77.8/66.0/65.6 in both).

**Windows idle wait** (`src/hooks/render_idle.cpp`). Windows now waits with the same policy as Linux: spin
20 us, yield up to 200 us, then sleep in 50 us steps until a frame, a message or a job, or 1 ms. Each step
waits on a high-resolution waitable timer (`CreateWaitableTimerExW` with
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`; `Sleep` and `SleepEx` count whole milliseconds). Windows before 10
version 1803 has no such timer: the log says so once, and each step yields (`SwitchToThread`) instead. The timer
wait is not alertable, so APCs queued to the thread run at the game's next alertable wait (its `SleepEx`),
within about 1 ms. `FORZA_RENDER_IDLE=spin` still keeps the game's loop. It compiles with clang-cl (the TU4 and disc
Windows cross builds); not run on Windows yet.

**Measured** (Linux, TU4, `tools/perf_scenario.sh` as in #234, `frame_rate` 30, one build, A =
`FORZA_GAMMA_TRANSITION=game`, B = the change, alternating, BENCH lock held, all runs CLEAN; CPU ms per guest
frame in the scene, mean [min-max]):

| Scene | Mode | fps | p99 ms | All threads | Render thread | Ramp functions on it |
| --- | --- | --- | --- | --- | --- | --- |
| free roam (3 runs each) | A | 30.0 | 35.1 [34.8-35.3] | 19.0 [18.6-19.6] | 2.74 [2.64-2.82] | 0.39 [0.33-0.43] |
| free roam | B | 30.0 | 34.9 [34.4-35.4] | 18.2 [16.5-19.5] | 2.21 [2.17-2.25] | 0 |
| main menu, DLC notice (3 runs each) | A | 30.0 | 33.8 | 6.45 [6.37-6.50] | 0.39 [0.38-0.41] | 0 |
| main menu, DLC notice | B | 30.0 | 33.8 | 6.46 [6.42-6.52] | 0.40 [0.39-0.41] | 0 |

- Free roam: the render thread uses 0.5 ms less per frame (19%). The ramp's own functions were 0.39 ms of it
  (`pow` and its helpers, the ramp builders, the D3D setter); the rest went with the pass's other work around
  them. All threads vary more between drives than that.
- Idle passes go from about 730 to 4,700 a second, because a pass without the ramp takes about a microsecond, so
  more fit in the first 200 us after each frame (the spin and yield phase, the same CPU time). The waits and
  the time waited (54-56%) do not change.
- The DLC notice has no gamma transition yet, so it does not change there.
- Not measured: Windows. There the change matters more: with the game's loop the render thread built the ramp
  in every pass between frames.

## Flush pending Vulkan shader-cache writes before waiting (#49)

Patch 0064 completes a pending stdio flush before the storage writer sleeps,
and drains shader/pipeline records queued before shutdown. Without it, a final
flush request could consume its notification and go to sleep with buffered
records, so hard exits left the next run without those cached states.

The Windows regression harness executes the actual StorageWriteThread body with
real files, a worker thread, one queued shader and pipeline, then 64 pipeline
records queued immediately before shutdown. Before: the one-second flush check
fails and shutdown drops the queued records. After: both checks pass, including
all 65 pipeline records. This is a cache-persistence fix, not an FPS claim or a
fix for every freeze in #49. No GPU-specific commands or policies change; AMD
runtime validation remains with the maintainer. The ordered standalone SDK
patch stack applies to bd833a2.

## NVIDIA Vulkan resolve-readback overhead (#49, 2026-10-15)

SDK patch 0065 avoids repeating GPU readback work in `readback_resolve=some`
after that resolve key has a valid CPU copy. The regular GPU resolve and
cross-scale synchronization still run. The first CPU copy is unchanged;
the validity flag is set only after the guest-memory copy succeeds. Keys include
the address, byte length and format signature. Reused entries still update their
last-used frame, and eviction discards the flag along with the buffers, preserving
the existing refresh-after-inactivity behavior. `fast` and `full` are unchanged.

Previously `some` skipped the repeated CPU `memcpy`, but still dispatched the
downscale, copied into host buffers, and inserted transfer/host barriers on every
resolve. This was substantial overhead on the tested NVIDIA system. Disabling
readback entirely is a diagnostic control, not the fix: it removes CPU results
needed by car-picture generation.

RTX 5060 Ti, driver 617.14, Windows 11, Vulkan/FIFO, 120 FPS cap, 1440p output,
16x AF and warmed persistent shader storage. No other game or build ran during
measurement. Each run used the same copied save and the festival throttle sequence
described below. These are short scene tests, not race-wide performance claims.

| Native 1440p scene and HUD | Guest FPS | Renderer p99 (ms) |
| --- | ---: | ---: |
| Before: `some`, incomplete-frame skipping on | 52.9 | 22.8 |
| Before: `some`, incomplete-frame skipping off | 52.4 | 23.4 |
| Diagnostic: readback `none` | 68.1 | 18.3 |
| Fixed: readback `some` | 68.8 | 18.2 |

The table uses seconds 30-54. A new pipeline still caused a roughly 185 ms hitch
near second 30 in the fixed native run; improving readback does not eliminate
first-use shader compilation. The measured interval had zero renderer gaps over
50 ms after the fix versus four before it. Do not treat this as all of #49 being
resolved, or as a fix for missing story cutscenes or broken car thumbnails (#50).

The DLSS Performance compatibility check retained 1280x720 scene rendering,
native 2560x1440 HUD and the 120 FPS cap. A before/after repeat measured
81.4 FPS / 17.0 ms renderer p99 before, versus 117.6 FPS / 12.7 ms afterwards.
The first fixed run measured 118.5 FPS / 12.1 ms. Neither fixed measurement had
renderer intervals over 25 ms in seconds 30-54. Both reported active NGX and
native HUD rendering without evaluation failures. The installed renderer was
updated only after building and validating the fix; the previous DLL and matching
capability metadata were backed up. The patch also applies independently on the
public main SDK patch series, without depending on the DLSS work.

The table above is the earlier integration measurement, retained with its
original method and limits; it is not a fresh measurement of this split branch.
The standalone patch applies to main without DLSS, ultrawide or thumbnail changes.
The readback key, first successful CPU copy and eviction lifetime are unchanged.

## NVIDIA readback cache-miss stalls (#49, 2026-10-16)

The redundant-transfer fix does not remove the first CPU copy after a readback
cache miss. A warmed Horizon Heats race still produced intermittent long frames.
A diagnostic run with `readback_resolve=none` removed these spikes, without
improving average FPS. Disabling readbacks is not the fix: car-picture generation
requires CPU results.

Private timing scopes identified repeated 16 KB resolve readbacks at cache misses.
The entries had been evicted after 61 inactive frames. The measured slow calls
spent about 33-35 ms in `CheckSubmissionFenceAndDeviceLoss`, under
`IssueCopy_ReadbackResolvePath`; these were not thumbnail overrides or new pipeline
compilations. That wait was added to the normal frame workload.

Patch 0069 uses two queued frames by default on NVIDIA, limiting CPU lead before
such a wait. It keeps the three-slot resource ring and the existing fence-based
retirement; AMD and other vendors retain the previous three-frame default. The
copy, its first-result synchronization, readback keys and cache lifetime are
unchanged. `vulkan_max_frames_in_flight=3` restores the previous queue depth;
positive values are clamped to 1-3 and nonpositive values select the vendor
default. Restart after changing this option. Queue depth 1 is exposed for
diagnosis, but was not benchmarked here.

RTX 5060 Ti, driver 617.14, Windows 11, Vulkan/FIFO, Windows set to 2560x1440
at 360 Hz, native 1440p scene and HUD, 16x AF, 120 FPS cap, DLSS off. The comparison
uses main `75149f1` (including the shadow and readback fixes), a copied save,
eight drivers in Horizon Heats, and the same scripted road route. Each row is one
60-second measured driving interval after a race warm-up. No build or other game
ran during measurement; screenshots were taken outside the interval. AI positions
and traffic interactions vary, so these are route tests, not identical frames.
All measured intervals created zero new pipelines.

| Variant | Presented FPS | p99 (ms) | Worst frame (ms) | Frames over 50 ms |
| --- | ---: | ---: | ---: | ---: |
| Main, three-frame queue | 59.35 | 22.04 | 51.20 | 7 |
| Diagnostic: readbacks disabled | 57.91 | 21.89 | 23.75 | 0 |
| Two-frame prototype | 58.74 | 21.79 | 38.40 | 0 |
| Patch 0069, NVIDIA automatic default | 59.00 | 21.56 | 35.13 | 0 |
| Same patch binary, forced three-frame queue | 59.49 | 21.61 | 54.50 | 11 |

The earlier three-frame race pass measured 59.32 FPS, a 54.85 ms worst frame and
11 intervals over 50 ms. The queue change reduces the long waits; it does not
eliminate every frame over 25 ms or make native 1440p reach 120 FPS. Validation
is limited to this NVIDIA system. Other vendors' default queue depth is unchanged,
but no AMD hardware was tested here. The timing scopes and captures are private
diagnostic files, not part of the SDK patch.
## Scaled texture upload cost (#101)

Issue #101 reports about 40 FPS at render scale 2 on an RTX 4060, even when
lowering output resolution or anisotropic filtering. Scale 2 still renders the
game at 2560x1440 when the presentation window is lowered to 1920x1080. Output
resolution alone therefore does not reduce this rendering or texture-upload work.

On an RTX 5060 Ti (driver 617.14, Windows 11, desktop 2560x1440 at 360 Hz), a
copied TU4 save parked in free roam was GPU-bound at scale 2. The 120 FPS cap,
DLSS off, 16x anisotropic filtering, Vulkan, and native 1440p scene/HUD were held
constant. Initial controls measured 119.7 FPS at scale 1, 59.8 at scale 2, 60.3
with scale 2 and 1080p output/4x filtering, and 58.8 with IMMEDIATE presentation.
Those four measurement windows compiled no new pipelines. Changing the cap to
60 also measured about 59 FPS. This did not reproduce a fixed 40 FPS limit.

A three-frame RenderDoc capture attributed 13.34 ms of 47.68 ms of GPU command
time to buffer-to-image copies. The scaled loader first untiled data into a
scratch buffer, then copied it into the sampled image. Patch 0071 fuses those
steps for eligible NVIDIA textures, writing identical bits through an R32_UINT
storage view. It does not change render scale, texel format, HUD resolution,
texture filtering, frame pacing, or CPU readbacks.

The final comparison used main `a428cec` plus 0071, the same renderer binary
with `vulkan_direct_scaled_texture_upload` false/true, the same copied save,
scripted menu input, and physical controller/keyboard driving excluded by a
private test harness. Each session warmed for 70 seconds and measured for 45:

| Upload path | Presented FPS | p95 frame time | p99 frame time | Worst frame |
| --- | ---: | ---: | ---: | ---: |
| Original scratch buffer + copy | 59.68 | 19.99 ms | 21.38 ms | 41.71 ms |
| Direct storage image | 68.89 | 17.50 ms | 18.56 ms | 35.60 ms |

That is a 15.4% FPS gain in this scene. The original measurement compiled no new
pipelines; the direct measurement compiled two at about 77 seconds. Taking the
same final 30-second window (85-115 seconds) in both runs, with no new pipeline
compilations, gives 59.73 versus 68.97 FPS, and p99 21.36 versus 18.20 ms. These
are local measurements on a 5060 Ti, not confirmation of the reporter's 4060
configuration. Keep #101 open until that configuration has been retested.

The path is limited to tiled, scaled, single-layer 2D base levels in RGBA8,
packed 10-bit color, or R32 float. Other vendors and texture layouts retain the
original path. Storage format support is checked, allocation/view failures fall
back, and `--vulkan_direct_scaled_texture_upload=false` disables it for comparison.

The opt-in GPU check compares the original compiled loader against the new image
shader for independent X/Y scales 1-7, all four endian modes, aligned and ragged
extents, a source offset, and canary pixels outside the destination rectangle.
All 1,176 cases passed on this NVIDIA device. It needs the optional Python
`vulkan` package and an SDK with the patch series applied:

```sh
python tools/test_vulkan_scaled_texture_upload.py --sdk /path/to/rexglue-sdk
```

The final renderer also reached free roam with official NGX Super Resolution
(1280x720 scene, 2560x1440 HUD/output) and DLAA (2560x1440 scene/HUD/output).
Both features initialized successfully and the captured output was inspected;
these were compatibility checks, not additional performance comparisons.

The GLSL source and its precompiled SPIR-V header are included in 0071; no shader
compiler is required when building the game. To regenerate the shader binary,
compile `texture_load_32bpb_scaled_image.cs.glsl` with glslang (`-V -S comp`) and
convert the resulting little-endian uint32 words into the checked-in header.
