# BC-250: ReImagined and Pinyon performance investigation

## Scope and evidence

Report: CachyOS, Mesa 26.2.4, standalone ReImagined 0.1.17 at 35–42 fps;
Pinyon 0.5.1 and 0.6.0 reportedly hold 60 fps at 1440p. The reporter subsequently confirmed the same result on 0.1.23, with 40 GPU CUs
unlocked and six CPU cores. Aggregate CPU/GPU utilization was below 100%;
that does not exclude a serial CPU bottleneck or synchronization wait. We have no BC-250 trace or local BC-250.
This is a source comparison and an experimental build option, not a verified
fix for that machine.

Compared ReImagined 0.1.17 (`1a14021`), 0.1.23 (`766f276`), and main
`323a3b9`, against Pinyon 0.6.0 (`9331e453e9850d0fcc1c1606dee95db834a9d317`)
and its pinned ShiftGlue SDK (`79d3e6fae4b79e00def48a595c80f0eaa47fd2b6`).
Pinyon 0.5.1's Linux setup and CMake files were also checked: they already
select `-mfma -ffp-contract=off` on supported hosts. The FMA difference
therefore predates 0.6.0 and is relevant to both reported Pinyon versions;
the rest of 0.5.1 has not been audited separately.

An unchanged frame rate when changing the launcher's resolution **does not by
itself establish a CPU bottleneck**. ReImagined's ordinary resolution setting
selects the guest video/window mode; `resolution_scale` controls scene raster
size (temporal upscaling and ultrawide add their own sizing). Check the actual
internal dimensions, output dimensions, upscaler and MSAA in both programs.
The Linux GPU detector recognizes AMD by PCI vendor, not a model whitelist;
we found no BC-250-specific performance cap.

Follow-up [issue #174](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/174)
reports no improvement at render scales 1 and 2 as well as guest resolutions
720p through 4K, and a roughly 1 GB game log. That strengthens the case for
profiling CPU work, waits and logging overhead; it does not identify the cause.
Current main (`5d246b0`) adds a warning that trace/debug logging lowers frame
rate. Confirm the reporter's log level and repeat at the normal level before
attributing the difference to code generation or hardware recognition. No
normal-level BC-250 comparison or FMA runtime result is available yet.

## Material differences

| Area | Pinyon 0.6.0 | ReImagined / implication |
| --- | --- | --- |
| Generated CPU math | Linux setup selects an FMA or x86-64-v3 build when supported, keeping `-ffp-contract=off`. | SDK baseline uses SSE4.1; explicit `std::fma` becomes a math-library call. Our existing [CPU profile](perf.md#cpu-time-per-frame-45-2026-10-16) identifies this in audio. A small, isolated experiment is justified. |
| Generated register state and calls | Its SDK keeps more guest register state in locals, folds constants, specializes calls, and maps guest job fibers to host fibers. | Potentially important CPU savings, but these change the guest ABI/resumption protocol. Port with reference-vs-optimized execution checks for both disc and TU4; copying flags alone is unsafe. Pinyon's research also records a correct nonvolatile-register-local experiment that was slower. |
| Renderer | A Forza-specific renderer reduces generic Xenos packet, EDRAM and resolve work. | ReImagined uses the general Xenos plugin with its fixes. [PR #157](https://github.com/Dyotson/ForzaHorizon1-ReImagined/pull/157) already develops guarded native passes; it is not yet a complete replacement. Coordinate there rather than creating a competing renderer. |
| Code layout | Linux x86-64-v3 builds use the small code model. | SDK uses `-mcmodel=large`. Smaller address sequences are worth investigating, but executable/module layout and relocations must be checked before changing this. |
| Thread placement | SDK's `linux_thread_cores=auto` detects a Steam Deck and applies a measured layout; it leaves other machines alone. | This Deck optimization does **not automatically apply on a BC-250**. Collect thread load/topology first; copying the Deck's CPU numbers can make another APU worse. |
| Audio | Stereo mixing and native DSP work reduce guest audio work. | FMA is a first step. Profile FMOD mixer/DSP separately before porting hooks; outputting stereo alone does not prove the guest surround mixer stopped doing work. |
| Pipeline compilation | Parallel compilation and asynchronous cache writes reduce cold-cache stalls. | [PR #163](https://github.com/Dyotson/ForzaHorizon1-ReImagined/pull/163) already covers startup warm-up/cache work. Hitches and sustained warm-cache 35–42 fps need separate measurements. |

Primary references:

- [Pinyon 0.6.0 release](https://github.com/arcanite24/pinyon-shift/releases/tag/v0.6.0)
- [CPU baseline selection](https://github.com/arcanite24/pinyon-shift/blob/v0.6.0/tools/pinyon_setup.py)
  and [compiler configuration](https://github.com/arcanite24/pinyon-shift/blob/v0.6.0/cmake/PinyonShiftRexGlue.cmake)
- [0.5.1 CPU baseline selection](https://github.com/arcanite24/pinyon-shift/blob/v0.5.1/tools/pinyon_setup.py)
- [Pinyon generated-code research](https://github.com/arcanite24/pinyon-shift/blob/v0.6.0/docs/NEXT_LEVEL_RESEARCH.md)
- [Pinned SDK thread placement](https://github.com/arcanite24/shiftglue-sdk/blob/79d3e6fae4b79e00def48a595c80f0eaa47fd2b6/src/core/threading_posix.cpp)

Pinyon's published instruction counts and Deck CPU reductions are evidence for
prioritizing investigations, not measured ReImagined or BC-250 speedups.

## Experiment: hardware FMA in generated code

`FORZA_GUEST_FMA=ON` adds `-mfma -ffp-contract=off` to the generated main
module and generated facade targets, including their precompiled headers.
The host/bootstrap and SDK retain their normal baseline. This is initially
limited to Linux x86-64 with Clang/GCC and defaults **off**.

- Explicit `std::fma`/`std::fmaf` can lower to hardware instructions.
- Separate multiply/add expressions must stay separate; no fast-math or
  global CPU-native tuning is added.
- An early ELF constructor in the baseline host checks both AVX and FMA
  through compiler CPU detection, including OS AVX state support, and the
  SSE4.2/POPCNT features also permitted by Clang's `-mfma` flag. It runs
  before normal generated static initializers, prints a rebuild instruction
  and exits on an incompatible machine.
- This is a build-time choice, not a `forza.toml` graphics setting. Do not
  distribute an FMA build as the portable release or enable it by default
  until game and affected-hardware validation are complete.

After configuring a local game build with the normal SDK and title-update
options, change only this option for an A/B/A comparison:

```sh
cmake -S forza-recomp -B /path/to/game-build -DFORZA_GUEST_FMA=OFF
cmake --build /path/to/game-build --parallel 4
# Measure baseline; retain its logs outside the source tree.
cmake -S forza-recomp -B /path/to/game-build -DFORZA_GUEST_FMA=ON
cmake --build /path/to/game-build --parallel 4
# Measure FMA; then repeat OFF to check run-to-run drift.
```

Use the same generated code, patched SDK, title update, copied save, route,
internal/output resolution, frame cap, AA, upscaler and warm shader caches.
Record average fps, frame-time p95/p99, per-thread CPU time, CPU/GPU clocks,
temperature and GPU busy time. Take at least three runs per variant and
record contention. Do not use an active personal save or public server.
`RECOMP_PERF=1`, `FORZA_FRAMETIME_LOG=1`, and the existing
`tools/perf_scenario.sh` / `tools/cpuprof_report.py` can supply frame and CPU
evidence. Follow [the profiling guide](perf.md) for their prerequisites.

### Reproducible arithmetic check (no game data)

```sh
cmake -S forza-recomp/tests/guest_fma -B /tmp/forza-fma-check -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DFORZA_GUEST_FMA=ON
cmake --build /tmp/forza-fma-check
ctest --test-dir /tmp/forza-fma-check -V
```

Repeat in a separate directory with the option OFF. The test compares explicit
fused operations and separate multiply/add for float/double across four rounding
modes, all four FTZ/DAZ combinations, special values and seeded random bit
patterns (4,334,912 comparisons). Non-NaNs must match bit for bit, including
signed zero; NaN payload selection and floating-point exception flags are not
verified. A default-priority initializer also exercises guest math before main,
after the early CPU guard.

On an i9-13900K under Fedora WSL, Clang 22.1.8, both variants passed; the
optimized check also passed with GCC 16.2.1. Assembly
inspection shows FMA instructions in the optimized explicit operations, separate
multiply/add instructions in the nonfused operations, and no AVX/FMA instructions
in the guard. Fault injection with gdb (clear compiler CPU feature bits at guard
entry) exits with status 1 before the generated-code initializer is called.
This does not replace an actual unsupported-CPU/OS test.

After the game builds finished, seven alternating-order runs of 10 million
iterations gave a small serial difference (8.29 ms baseline, 7.77 ms FMA).
Four independent accumulators fell from 57.23 to 7.51 ms. These synthetic loops demonstrate that
call/ABI overhead can matter when work overlaps; they predict neither the
audio thread's improvement nor game fps. BC-250 runtime validation is pending.

### Game build and packaging checks

- Both disc (`FORZA_TITLE_UPDATE=0`) and TU4 (`4`) compile and link with the
  option ON, using SDK `0.10.0.24-dev.gbd833a2` and Clang 22.1.8.
- The full TU4 executable also rejects injected missing CPU features in its
  early guard. The guard's object contains no AVX/FMA instructions.
- Unsupported-platform configure is rejected; the portable arithmetic check
  passes with the option OFF.
- Launcher/Flatpak source enumeration includes the new CMake/host files and
  excludes private generated code.

Game boot, driving, graphics and audio have not been validated for this build.
An attempted `--help` smoke check aborts in SDK CLI/path setup in both an
existing baseline binary and this executable; it is not counted as a boot pass.

## Next experiments, in order

1. Obtain the updated BC-250 baseline and a per-thread profile. Establish
   actual raster dimensions and whether the limiting work is simulation,
   command processing, audio, GPU execution or a synchronization wait.
2. Compare the FMA build on that device; listen for audio problems, test driving
   and menus on disc and TU4, and check frame times rather than only averages.
3. If command processing dominates, prioritize descriptor/binding work and
   the existing native-renderer effort. If guest CPU work dominates, evaluate
   register/stack/call optimizations with differential correctness checks.
4. Test code-model changes and topology-aware thread placement independently.
   Keep experiments reversible and reject changes whose improvement is within
   repeated-run noise.
