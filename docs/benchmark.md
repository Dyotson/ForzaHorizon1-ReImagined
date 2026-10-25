# In-game benchmark (Title Update 4)

Open **BENCHMARK** on the main menu, then choose **RUN BENCHMARK**. The game restarts into an offline, independent copy of your saved profile and opens Recaro Rush (FR02). Its native race AI drives the player car; you do not need to drive to the event marker or operate the race menu.

The current graphics flags are preserved across the benchmark restart and restored when returning to the normal game.

The first 15 seconds after the car moves are settling time. The next 60 seconds are measured. Results show average FPS, 1% low, P99 frame time and a frame-time graph. **RUN AGAIN** starts from another fresh copy; **BACK** returns to the normal game session. B cancels an active run.

## Comparing settings

Use a profile that has completed the festival introduction. Keep the same car, camera, difficulty, output resolution and frame cap between runs. Warm the route once before recording comparisons. Settling time does not precompile all shaders, and native AI traffic and collisions can vary. This is a fixed event with the current player car, not a fully deterministic replay or a fixed-car hardware ranking.

The graph preserves each bucket's longest interval. Measurements come from guest VdSwap intervals and include real stalls; they do not count generated frames or the display's refresh rate. The 1% low is the reciprocal of the mean of the slowest ceil(N/100) intervals. P99 uses the nearest-rank interval. Losing the race session or remaining stationary for ten seconds invalidates the run.

## Files and save isolation

Each run writes an independent profile and its reports under `user_data_root/benchmarks/recaro-<timestamp>/`:

- `session.txt`: route, output size, AA/upscaler and frame cap.
- `frames.csv`: elapsed time and each measured frame interval, in milliseconds.
- `result.json`: validity, timing summary and stall counts.

Writable profiles are copied byte-for-byte; no profile is hard-linked. Installed DLC packages may be hard-linked as immutable inputs. No downloaded profile, game assets or screenshots are needed in the source repository.

On Windows, the DLC setup uses extended filesystem paths. Adding the benchmark's
timestamped directory to a deep DLC installation can exceed the traditional 260-character
path limit: `create_hard_link` then fails with "The system cannot find the path specified",
and the setup prompt reappears after the restart (#374, related #364). A filesystem-only
fixture reproduced the error at a 285-character destination and succeeded with the
extended path form. The regression links a deeply nested synthetic DLC file, verifies
the linked bytes and checks that writable profile copies remain independent. It also
covers extended-path reuse and UNC path conversion without accessing a network share.
This validates the setup failure path; a complete benchmark run still needs a game retest.

The #405 screenshot shows the same ordinary-path hard-link failure, and its attached
log records TU4 and 21 DLC packages despite the report form saying none. That capture
predates #399; it does not establish a separate no-DLC failure.

Closing setup with B and reopening it needs a separate confirmation press. Input is
only polled while the dialog is visible, so remembering the previous screen alone
cannot detect a close/reopen cycle. Each dialog entry resets the pad edge history;
the opening A press and a continued hold cannot also start a benchmark. This addresses
the immediate second-open restart reported in #374, separately from its DLC setup error.

## Focused tests

Compile and run `forza-recomp/tests/benchmark_stats_test.cpp` and `benchmark_profile_test.cpp` as standalone C++20 programs. They check time-weighted FPS, percentile/1% low behavior, real stalls, invalid samples and save-copy independence. End-to-end validation additionally needs the user's TU4 game data and a disposable saved profile.

With the pinned ReXGlue SDK installed, the focused CMake suite also runs the
actual title-selection hook against synthetic guest memory and checks controller
capability routing from current main:

```sh
cmake -S forza-recomp/tests/benchmark -B build/benchmark-tests \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$REXSDK"
cmake --build build/benchmark-tests --parallel 2
ctest --test-dir build/benchmark-tests --output-on-failure
```

The session fixtures compile the actual session implementation for base and TU4,
stubbing only the process restart and race callbacks. They check close/reopen input,
one restart after a fresh confirmation, invalid and consumed request markers, setup
failure without an automatic retry, independent copied saves, settings restoration
and the return marker. They start no game or GPU. For a rendered retest on a disposable
profile, open Benchmark, press B, then reopen it with A and hold A: setup must remain
visible until release and another A press. That fresh press must restart once and
start the copied offline session. Record the build, input method and original-profile
hashes separately from these fixtures.

The title regression repeats each stock choice 20 times with both one and two
extra rows, checking local/world transforms and child geometry after every
modal, plus benchmark setup, departure and restoration. It does not exercise
native input repeat, render labels or emulate a physical controller. The
release workflow runs these tests in `sdk-linux`.

For scripted title-menu navigation, use explicit short taps such as
`FORZA_AUTOPRESS="28:DOWN:0.08,29:DOWN:0.08,30:DOWN:0.08,35:DOWN:0.08"`
after reaching the menu. The default 0.3-second hold can trigger native button
repeat after about 0.2 seconds, even with a full second between presses. With
0.08-second taps, four Down presses from Single Player should select Multiplayer,
Horizon Rally, Graphics, then Benchmark. Check the hold duration when diagnosing
skipped rows; test timing is relative to the first input poll, not process launch.

For the maintainer's runtime retest (#138 and #223), start offline on a disposable
profile copy. From Single Player, use three distinct Down taps to reach Graphics
and a fourth to reach Benchmark, then navigate back through all five rows.
Repeatedly select and dismiss each stock choice that opens a dialog, including
Horizon Rally's Xbox LIVE profile dialog and Multiplayer Unavailable. Check that
each highlighted label remains on its own row while the dialog is open and after
dismissal. When Single Player leaves the title, return to a fresh title screen
before repeating. Also open Graphics, go Back and repeat the navigation. Record
physical-controller testing separately from scripted taps, including OS, GPU,
build SHA and hold duration for scripts.
