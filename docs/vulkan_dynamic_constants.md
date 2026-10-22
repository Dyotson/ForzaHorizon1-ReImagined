# Vulkan dynamic uniform constants experiment

This experiment targets the ordinary Vulkan command processor's per-draw
constant-buffer descriptor writes. Dynamic uniform offsets let multiple draws
reuse descriptors while selecting their existing constant-buffer slices.
Guest shaders, constant packing, textures, samplers, guest code generation and
fence-wait policy retain their existing behavior.

## Mechanism and lifetime

`FORZA_VK_DYNAMIC_CONSTANTS=1` selects dynamic uniform descriptors at device
setup. The default is off. The five constant bindings retain their explicit
ranges, with descriptor base offsets zero; each recorded bind includes the five
aligned offsets into the existing immutable upload slices. The deferred stream
already copies the offsets into the command record. The Vulkan API contract is
described in the [Khronos guide](https://docs.vulkan.org/guide/latest/descriptor_dynamic_offset.html).

An at-most-256-entry memo uses every buffer handle and range as its key. It
ignores offsets, which are supplied per draw. It is cleared between guest frames
and when transient pools are cleared. A miss still allocates a normal transient
set from a dedicated dynamic-uniform pool; entries beyond the cap remain misses.
Only newly allocated sets enter the existing fence-retired ownership queue.
Cached sets are never rewritten while GPU commands can reference them.

Offsets larger than Vulkan's 32-bit dynamic-offset field fail explicitly.
Allocation retains the existing uniform alignment and buffer-range rules. Five
dynamic uniform bindings fit Vulkan's mandatory minimum of eight. No optional
descriptor-indexing or push-descriptor feature is required. Single-transient
uniform layouts used by other passes keep their original descriptor type.

## Validation

The original private Release SDK GPU build passes. The original patch series
contained 66 patches including this experiment. A standalone GPU test
under Khronos synchronization validation checks 1,600 compute dispatches across
five dynamic bindings, per-dispatch offsets, changed buffer/range variants and
eight fenced frames; all outputs match, with zero validation errors. This test
checks the Vulkan data/synchronization contract, not application-memo hit rates.

An excluded AI-driven candidate warm-up also completes with Khronos API
validation enabled and no reported VUID/validation errors. Validation is expensive
in this workload, so its FPS and CPU costs are not performance scores.
`FORZA_VK_BINDING_STATS=1` records draw, new constant-set and memo-hit counts;
it is enabled only in excluded warm-ups.

In the candidate warm-up's four counter windows emitted during the AI interval,
5,687,749 constant-set requests hit the memo and 430,454 allocate a new set:
92.96% reuse. Counter windows may straddle the AI boundaries; these counts are
not an exact whole-segment total. The same constant-buffer payload uploads still
occur, and texture descriptors remain on their original path.

### Rebase and overlap audit

The branch is rebased onto main `56e814d` (2026-10-19). All 67 public SDK patches,
including 0092, apply to pinned SDK `bd833a2`. Patch number 0092 is unused by
the 16 open PRs inspected at this revision.

The SDK series was also checked with each SDK-changing open PR independently:
#116, #133, #136, #157, #163, #169 and #170. Adding 0092 before or after each PR's
additional SDK patches produces the identical Git tree, with no application
conflicts. HDR, macOS and native-renderer patches touch the same command-processor
files in different hunks. Other open PRs introduce no SDK patches. This audit
checks interactions with 0092; it does not establish that every other PR's SDK
series can be merged together, or validate their combined runtime behavior.

The [rebase validation record](benchmarks/vulkan-dynamic-constants-rebase-2026-10-19.json)
pins the inspected main/PR heads and resulting SDK trees.

The exact rebased public series also passes a fresh Windows x64 Release build of
`rexgpu-xenos` and its `rexruntime` dependency, with Vulkan and D3D12 enabled.
Tracked SDK sources are verified against the recorded patched tree; this build
does not reuse the benchmark runtime or its private source extras.

The earlier performance numbers below retain their original source and binary
provenance; they are not
new measurements of the rebased public SDK. Integrated game/race validation on
the rebased series remains pending, so the PR stays draft and the option stays off.

## Benchmark protocol

Both modes use the same executable and DLLs, with register-local codegen and
exact physical mapping disabled, and the original Windows yield fence policy.
The AI instrumentation is reused from benchmark commit
`ec7300244e369d2802b1112ecd02bc681cb8cf94`, identically in both modes.

The native race AI drives the Evo in eight-car Recaro Rush / Montano Dirt Cross.
After the movement gate and 15 seconds of settling, the native benchmark scores
60 seconds. Process CPU uses a separate aligned 30-second window starting ten
seconds into that scored segment, divided by its own guest-frame count. This
is a driven segment with variable traffic, not a deterministic replay.

Hardware: i9-13900K, 64 GB, RTX 5060 Ti, driver 617.14. The display is verified
at 2560x1440, 360 Hz. The game renders 1280x720 at 1x, target 240 FPS, no upscaler,
VSync off and verified IMMEDIATE presentation, with unrestricted affinity.
Two excluded warm-ups precede alternating A/B, B/A pairs. Scored runs have
validation, descriptor counters, stack sampling and GPU telemetry disabled.
The runner checks foreground coverage and competing games/compilers. Every run
uses an independent offline copy of the same player-owned seed.

Both members of original pair 1 were conservatively excluded because a short
source audit could have overlapped the baseline's last 1.31 seconds of FPS
measurement. Its CPU window had already ended. Pair 6 replaces that entire pair;
the five retained pairs are 2–6, beginning with candidate/baseline order. The
original measurements and exclusion details remain in the local raw data.

The SDK snapshot starts from pinned `bd833a2` with the carried patch set. It also
retains pre-existing scaled-resolve/platform changes shared by both modes;
their source-diff hash and file list are recorded locally. This is an A/B of
one flag in that frozen snapshot, not a runtime qualification of the exact latest
main tree. The exported SDK patch contains only the constants-binding changes.

## Results

The [portable results](benchmarks/vulkan-dynamic-constants-2026-10-19.json)
contain all ten retained scores, pair ordering, binary hashes, validation and
snapshot provenance. Arithmetic means across five runs per mode:

| Metric, 720p | Original descriptors | Dynamic constants | Change |
| --- | ---: | ---: | ---: |
| AI segment FPS | 97.95 | 105.54 | +7.7% |
| Process CPU ms / guest frame | 31.50 | 29.76 | −5.5% |
| GPU command thread CPU ms / guest frame | 8.84 | 8.28 | −6.4% |
| 1% low FPS | 76.01 | 76.25 | +0.3% |
| p99 frame time, ms | 12.64 | 12.31 | −2.6% |
| Worst frame time per run, mean ms | 15.33 | 17.15 | +11.9% |

FPS improves in all five retained pairs (+4.3% to +12.3%). Process CPU cost per
frame improves in four pairs; pair 4 is essentially flat (+0.35%). The aggregate
1% low is flat and the mean worst frame is worse, so this does not establish a
frame-pacing improvement. Total process CPU usage averages 3.18 versus 3.21 cores:
the result is more throughput per unit CPU work, not lower total CPU use or power.
GPU command thread figures are thread CPU counters, not GPU time or wall time.

All ten scores have 100% foreground coverage, complete valid AI segments, no
competing game/compiler processes and zero new graphics-pipeline creation log
entries during the AI scored segment. The feature remains opt-in pending wider
race, hardware and integrated-source testing.

A separate single pair at 2560x1440 (2x rendering, candidate first) measures
69.73 → 70.80 FPS (+1.5%) and 38.65 → 37.11 process CPU ms/frame (−4.0%). GPU
utilization sampled once per second during the CPU window averages 98.0% versus
98.9%. This supports a GPU bottleneck at native resolution and shows a much
smaller throughput benefit there. One pair with telemetry is diagnostic evidence,
not a repeated performance estimate. Both native runs have complete AI segments,
100% foreground coverage and zero new pipeline-creation entries while scoring.
Six captures from retained pairs 2 and 6 and the native diagnostic were visually
spot-checked with no obvious rendering corruption. Their different traffic and
AI positions make them unsuitable for a pixel-identical correctness comparison.

## Reproduction

`tools/bench_vulkan_bindings.py` accepts the existing game/update/save/cache/output
arguments and the same runtime for `--baseline` and `--candidate`. Use
`--baseline-constants off --candidate-constants on --ai-race --rounds 5 --warmup all
--settle 10 --seconds 30 --frame-rate 240 --present-mode immediate --require-foreground`.
AI mode requires the existing benchmark hooks; this experiment does not duplicate
that feature. The eight-car seed used here also needs the input schedule
`8:A:0.08,18:START:0.08,28:A:0.08,48:X:0.2,56:A:0.2,62:A:0.2,90:A:0.2,101:A:0.2`.
The timings depend on the seed and build. `--binding-stats` adds warm-up counters;
`--validate-warmup --validation-layer-path PATH` validates the excluded dynamic
warm-up. Keep diagnostics outside scored comparisons.

The standalone GPU test builds from `tools/vulkan_bindings` with CMake, Vulkan
headers/import library and glslangValidator. CTest requires the Khronos validation
layer to be installed or supplied through `VK_LAYER_PATH`.

The runner's CLI smoke check verifies alternating modes and warm-ups, unchanged
resume, and rejection of changed constants/counter/validation controls on resume.

Raw build logs, provenance, captures, manifests, CPU/frame samples and AI reports
remain under `build/vulkan-bindings`. No binaries, generated game code, captures
or personal saves are included in the source change.

## Main integration, 2026-10-22

Integrated main `58bfc10`, including the Controls changes. Patch 0092 retains its
number and opt-in behavior. Its redundant `<cstdlib>` insertion is removed:
main's later device-loss patch 0104 supplies the include, so adding it earlier
would break the ordered series.

All 92 SDK patches apply in order to pinned SDK `bd833a2`. The exact public series
passes an isolated Windows x64 Release Vulkan/D3D12 GPU-plugin and runtime build.
The standalone dynamic-uniform GPU test and its shader compile. GPU execution,
synchronization validation and integrated game/race qualification are deferred
for this head. Earlier 1,600-dispatch validation and benchmark results retain
their recorded provenance; this integration supplies no new performance result.

A source-only cross-PR audit applies all 105 patches from current main and all
open SDK-changing PRs together, including #157's 0117-0126, #192's 0094 and #286's
0114-0115, with unique numbers. This is an ordered-application check, not combined
runtime qualification. Dynamic constants remain off by default and the PR stays
draft pending its existing runtime gate.
