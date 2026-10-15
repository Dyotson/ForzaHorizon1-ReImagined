# Performance

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
(warm cache), audio decode and readback are not what limits it; the main thread is.
