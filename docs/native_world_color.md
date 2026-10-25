# Native world-color composition

The separately selected `world-color-transfer` mode also bypasses the supported
R10 scene's EDRAM dump and untiling. See [Native scene transfer](native_scene_transfer.md)
for its conversion, lifetime, fallback and validation contracts. Measurements
below compare the original composition with `world-color`, without that transfer.

`FORZA_NATIVE_RENDERER=world-color` selects an experimental replacement for the
world's color finishing pass. It performs the edge-aware scene filter, bloom
combination, exposure curve, 3D color grading, vignette/tint and output color
matrix using authored vertex and fragment shaders. The HUD, labels and minimap
are composed afterward at output resolution.

This replaces the complete supported composition draw, including its rectangle
expansion and exposure lookup. World meshes, material lighting, reflection
rendering and the surrounding EDRAM/resolve pipeline remain on the existing
backend. It does not establish a complete native world renderer or a Metal port.

## Selection and profiling

The initial native-1440p gameplay capture contained four frames. GPU replay
counters identified substantial tiled world rendering and attachment transfer
work. The world-color composition shader also ran twice per frame over two
2560x720 regions. Its eight draws accounted for roughly 2.4 ms of replay GPU time
across that capture. These are instrumented replay timings, not live frame times. Another FH1 session
was present during this initial profile; it identifies work to investigate, not
an uncontended per-pass cost or a measured GPU saving.

The two regions are not interchangeable: the captured tint/exposure controls
differed. Each region therefore keeps its own parameter and input-quad snapshot.
The native pass preserves both regions rather than merging their controls.

`native_renderer_profile capture.rdc timings.tsv` collects the supported
`EventGPUDuration` replay counter and writes event IDs, milliseconds and action
names. It fails if the counter is unavailable or returns invalid data. Build it
alongside `native_renderer_capture` with the official RenderDoc headers/library.
Captures and generated output stay under `build/`.

## Resource contract

The Vulkan pass imports single-mip scene, noise, bloom and exposure views, a
single-mip 3D grading view, and the current single-sample color attachment. It
keeps the SDK's attachment ownership and downstream resolves intact; it does not
claim to remove those transfers. It owns its pipeline, samplers, parameter
buffers and immutable 48-byte GPU snapshots of the source rectangle.

Eight slots per attachment compatibility class are retired by waited submission
fences. Descriptors and parameters are not modified while their slot is in use.
Transfers occur at the draw's position in the GPU stream, before entering the
render pass. The source may subsequently be overwritten without changing an
earlier snapshot. Recording/submission jobs and GPU work drain before teardown.
The pass adds no device-idle wait.

The adapter recognizes one verified program pair and checks its rectangle,
constant maps, texture dimensions/signs/mips, color format, blending,
depth/stencil, queries and memory exports before recording native work.
Unsupported states or initialization/allocation failures retain the original
draw. The existing independent native depth, motion and resolve modes remain
unchanged; `combined` does not implicitly enable this experiment.

## Validation

- A private original-GPU fixture supplies the real input images, rectangle,
  controls and reference output. All 3,686,400 pixels of its two-region 1440p
  output match within one 10-bit channel step. 3,686,079 pixels match exactly.
  Shader binaries, input images, constants and reference output are not committed.
- The rectangle's fourth corner preserves the original add/subtract order.
  Reassociating that arithmetic changed the large dither seeds and produced
  visible differences at some edges; the corrected GPU comparison above covers it.
- Standalone synthetic GPU tests use independently owned output images and
  generated inputs. They check two different regions, source overwrites between
  snapshots, all vertex endian modes, scissor/padding preservation, output gamma,
  in-flight descriptor protection and fence reuse. Traditional render passes and
  Vulkan 1.3 dynamic rendering both run under synchronization validation.
- Parameter tests reject non-finite controls, invalid exposure ranges, invalid
  output rules and a singular exposure normalization.
- A copied-save AI race executed the live pass at native 2560x1440, with scene and
  HUD dimensions independently logged and final presenter output captured at
  2560x1440. No Vulkan validation errors were reported in this smoke session.

The fixture establishes this pass's output contract, not all scenes or graphics
settings. The live harness stops its own child after saving evidence; graceful
game shutdown has not been established by that run.

## Warmed native-1440p comparison

The RTX 5060 Ti 16 GB / i9-13900K development machine used the same public host,
GPU/runtime DLLs, copied save seed, AI race, shader cache and settings for every
run. World and HUD dimensions were logged as 2560x1440. SMAA, no temporal
upscaling, vsync, threaded Vulkan submission and a configured 120 FPS cap were
unchanged. The original scene's multisampled rendering remains in use.

One original and one native run warmed the caches and are excluded below. Four
subsequent runs used original / native / native / original order, each with
15 seconds settling and 60 seconds measured. No validation/capture layer,
screenshot readback or GPU replay ran during the measured intervals. A
once-per-second process audit found no other FH1 process in any timed run.

| Run | World-color path | Average FPS | 1% low FPS | P99 | Worst | >50 ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | Original | 57.60 | 39.03 | 19.23 ms | 36.63 ms | 0 |
| 2 | Native | 59.85 | 40.31 | 18.45 ms | 35.04 ms | 0 |
| 3 | Native | 59.38 | 40.39 | 18.39 ms | 34.65 ms | 0 |
| 4 | Original | 57.51 | 39.17 | 19.18 ms | 36.82 ms | 0 |
| Pooled | Original | 57.55 | 39.10 | 19.19 ms | 36.82 ms | 0 |
| Pooled | Native | 59.62 | 40.35 | 18.39 ms | 35.04 ms | 0 |

Pooled rates use saved frame intervals, not averages of FPS samples or
percentiles. Original: 6,905 intervals / 119.975746 seconds. Native: 7,153
intervals / 119.982836 seconds. The 1% low is the reciprocal mean of the slowest
ceil(N/100) intervals. CSV counts and summed times agree with the benchmark
reports, and recorded binary hashes match across all four runs.

This pair of repeated runs shows a 3.59% average gain and a 3.20% higher 1% low
for this scene. It does not establish the same gain elsewhere or isolate a GPU
cost: the remaining original setup, resolves and geometry are still present.
The cap is 120 FPS; achieved throughput is approximately 58-60 FPS. AI traffic
varies, and window focus was not continuously monitored or independently
verified for every run. This is a development comparison, not a deterministic
hardware ranking, and it does not measure `combined` mode.

Separate visual sessions captured moving gameplay at 2560x1440 from the final
presenter, with HUD, minimap and labels visible. They show nearby race moments,
not frame-identical replay; the independent original-GPU fixture above supplies
the pixel comparison. Screenshots and private evidence stay outside the repo.

All thirteen standalone tests pass locally: six CPU tests and seven GPU tests
with Khronos synchronization validation. The CPU-only configuration also passes
all six tests without Vulkan. The Windows SDK builds both with the canonical
native sources and with the standalone fallback; the complete public patch
series applies to its pinned base. Other scenes, cutscenes and render scales
still need broader regression coverage before enabling this mode by default.
