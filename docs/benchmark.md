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

## Focused tests

Compile and run `forza-recomp/tests/benchmark_stats_test.cpp` and `benchmark_profile_test.cpp` as standalone C++20 programs. They check time-weighted FPS, percentile/1% low behavior, real stalls, invalid samples and save-copy independence. End-to-end validation additionally needs the user's TU4 game data and a disposable saved profile.
