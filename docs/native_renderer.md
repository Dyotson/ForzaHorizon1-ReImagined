# Native renderer bring-up

## Goal and current boundary

Replace the Xbox 360 rendering contract incrementally with an explicitly owned
Vulkan scene graph. Keep the original world, reflections and UI intact while
moving to native attachments, resource lifetimes and pass scheduling. Preserve
independent scene/output resolution and compose the HUD at output resolution.

The current backend already executes Vulkan draws. It still interprets Xenos
state, replays EDRAM tiles, resolves into guest-addressed buffers and reloads
those buffers as textures. A final image blit or a new AA filter alone does not
replace that renderer. Removing those copies without tracking consumers can
invalidate reflections, minimap inputs, screenshots and CPU readbacks.

This branch contains development infrastructure and an opt-in replacement for
one motion-effect pass. It is **not a complete native world renderer**:

- `render_graph.h` assigns resource versions and records RAW, WAR, WAW and alias
  dependencies. A mutable attachment and an independent snapshot are distinct
  resources. Discarded contents, stale aliases and same-pass alias feedback are
  rejected by content validation.
- `native_renderer_capture.cpp` uses the official RenderDoc replay API to export
  operation/resource metadata from an existing capture. It does not export game
  shaders, images, vertex data or saves.
- `native_renderer_report.py` verifies event/dependency ordering and identifies
  small draws worth inspecting. A small draw is not automatically a world pass;
  HUD draws must be distinguished by shader, inputs, viewport and target state.
- `velocity_pass` owns Vulkan depth, motion and blur attachments. Its authored
  compute shader decodes packed depth/stencil, reconstructs per-object movement,
  and keeps unclamped temporal motion separate from shaped blur. It executes in
  the standalone GPU test and, through SDK patch 0079, in the game's draw stream.
  Patch 0081 resolves that pass's RGBA8 image directly into the existing guest
  texture buffer, bypassing its EDRAM dump. Geometry, reflections, other effects
  and HUD composition retain the existing renderer. The resource planner does
  not schedule these runtime passes yet.

The planner currently works at whole-resource granularity. Layout transitions,
queue ownership, subresources, native allocation and guest EDRAM alias mapping
are not implemented. Captured resources import their initial replay contents;
this does not establish how a native frontend will populate them. Exported graphs
therefore explicitly say `execution_ready: false`.

## Native depth and motion pass

The pass accepts a host parameter block containing 43 preceding-frame transforms,
projection parameters, blur response, UV orientation and motion scale. The caller
supplies a point-sampled RGBA8 array view in shader-read layout and reports GPU
completion using monotonically increasing submission IDs. Those IDs must come
from waited fences. Enqueuing work does not make its resources reusable.
Dimensions are limited to 32768 and the UV mapping supports identity or mirrored
axes. Atlas/subrect sampling needs a separate contract. Center texels are selected
with integer arithmetic: normalized point filtering at an exact 2:1 reduction can
choose opposite sides of a depth/stencil boundary because of coordinate precision.

`Prepare` reserves a free frame slot and updates its descriptors/constants on the
producer thread. `Record` consumes an immutable dispatch snapshot on the Vulkan
submission thread. Slots own separate attachments and constants; preparation
rejects unfinished slots and invalid parameters without recording GPU work.
Prepared dispatches must be submitted; abandoning a command stream requires an
idle-device reset of the pass. Consumers must return its images to GENERAL layout
before reuse. Before destruction the owner must drain recording jobs and wait
their submission fences. The pass does not wait on an externally owned device:
doing so can race another thread's queue submissions. Partially initialized,
never-submitted resources can be released without a device-wide stall.

Outputs are R32F depth, RGBA32F motion (XY in output pixels) and RGBA8 blur. The
four-component motion image is intentional during validation: it preserves full
precision without requiring `shaderStorageImageExtendedFormats`, which the
existing device setup does not enable. A supplied function table lets the pass
use the runtime's Vulkan loader. No NVIDIA library is required.

Synthetic fixtures test static and translated cameras, individual car/stencil
IDs, the world mask, all reachable object IDs, invalid transforms, singular homogeneous
coordinates, blur limits, two orientations and output sizes 73x41 and 2560x1440.
The odd size also exercises partial compute workgroups and readback alignment.
Every output pixel is compared with a CPU reference; closed-form reference tests
independently check expected pixel displacements and depth values.
Additional fixtures exercise a BGRA image-view mapping and an exact 2:1 reduction
in both orientations.

On an RTX 5060 Ti, all three Release tests passed with Khronos synchronization
validation enabled and zero validation errors. At 2560x1440, depth and quantized
blur matched within one quantized colour step; motion differed by less than
0.001 pixels. These
are synthetic correctness measurements, not a comparison with the game's actual
velocity attachment or a whole-frame performance result. No shader disassembly,
game textures, saves or generated binaries are included in the implementation.

A separate local replay comparison subsequently identified a motion draw at a
1280x720 viewport/scissor within a 2560x4096 RGBA8 attachment. Its 2560x1440 packed
depth texture uses a BGRA image-view mapping. Supplying that view and the captured
transforms to the native GPU pass produced **byte-identical RGBA blur output at
all 921,600 pixels**, repeated through both frame slots, with synchronization
validation enabled. This is a neutral-motion frame: its color channels contain
neutral velocity, while alpha still distinguishes cars from the world. It checks
real input format, sampling, object masks and output encoding; it does not prove
moving-camera equivalence. The original normalized-filter implementation differed
at 496 alpha edge pixels; integer center selection removed every difference.

The moving race exposed additional requirements. Blur reprojection must use raw
decoded depth, including values above one; only the exported temporal depth is
clamped. Stencil indices below 128 can reach transform 42, not just the first 25
entries. Zero products retain the legacy zero-times-infinity/NaN behaviour. A zero
homogeneous position and NaN/Inf results have different blur encodings, while
temporal motion rejects invalid projections. Unused non-finite matrix entries do
not invalidate an otherwise usable frame. These cases now have synthetic tests.

An offline GPU comparison using a moving-race fixture, including 17 NaN and 42
infinite projection results, matched all 921,600 car-mask pixels exactly and all
colour channels within one quantized step. This ran with Khronos synchronization
validation and zero errors. A private integration subsequently executed the
native pass before the original draw across four captured race frames: each
comparison had maximum error [1,1,1,0] and no pixels outside that tolerance.

The replacement prototype then removed the original motion draw. Its
capture shows a native dispatch, an image copy into the existing target, and the
usual resolve consumers. All four copies matched the native output exactly.
The recovered game output is 2560x1440 with the original HUD. This still uses the
legacy renderer for geometry, other effects, EDRAM resolves and HUD composition.
It is an incremental pass replacement, not a complete native world renderer.

Capture startup required a private runtime workaround: RenderDoc 1.46 on Windows
advertises `VK_KHR_portability_enumeration`, then rejects instance creation when it
is enabled. A standalone extension probe isolated that extension; omitting it
when RenderDoc is attached allowed the AI race capture. Validation reported
swapchain and queue-use errors at the capture boundary, so that run cannot establish
integration validation or performance. The workaround is not included in patch 0079.

A subsequent race ran without RenderDoc at native 2560x1440 scene/HUD resolution,
120 FPS cap and SMAA, with threaded submission and Khronos synchronization
validation enabled. The guarded bridge replaced over 1,300 draws, completed the
15-second settling and 60-second measurement stages with zero Vulkan validation
errors. The test harness terminated its child after the saved report; this does
not establish graceful shutdown validation. An earlier prototype reported an occlusion-query
scope error; the bridge now leaves draws inside active queries on the original
path. A transition-time `native_hud_seed` warning also occurs with the original
renderer. Validation substantially reduces frame rate; those results are not
performance measurements. Fractional scene inputs remain on the original path.

The complete patched Windows SDK also built with DLSS and FSR disabled, and its
GPU plugin executed the replacement in a freshly rebuilt public TU4 host. No NGX
runtime was loaded. A preliminary sequential comparison, with native 1440p,
SMAA, threaded submission and the cap configured to 120, measured:

| Pass | Average FPS | 1% low | P99 | Worst | Stalls over 50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original | 69.63 | 44.73 | 16.73 ms | 30.10 ms | 0 |
| Native with compatibility copy | 69.37 | 42.22 | 17.22 ms | 30.38 ms | 0 |
| Native direct attachment write | 69.96 | 46.98 | 16.39 ms | 29.34 ms | 0 |
| Native direct write and fused resolve | 69.55 | 42.13 | 16.78 ms | 30.12 ms | 0 |

These are functional comparison results, not a demonstrated speedup. The window
was covered by a pending Windows Firewall prompt, and AI traffic varies. More
runs with normal foreground conditions are required for performance conclusions.
Earlier local scores were invalidated: a stale development object counted each
benchmark frame twice. After rebuilding all public host sources and regenerating
the build, the CSV contains one interval per frame and agrees with renderer frame
counts. The figures above use only that verified executable.

The local fixtures, captures and screenshots remain volatile files outside the
repository. The benchmark preview runtime was restored after the experiments.
The SDK bridge and build integration are published in this branch. The feature
remains disabled by default.

## Enable the experimental runtime pass

The project SDK build scripts and release CI pass
`-DREXGLUE_FH1_NATIVE_SOURCE_DIR=<repo>/forza-recomp/src/platform/native_renderer`.
This compiles the canonical pass implementation and generates its shader header
in the SDK build directory. SDK configuration fails for an incomplete nonempty
source directory. Without this argument, the SDK builds a fallback bridge that
retains the original renderer. The native bridge and the fallback both compile
with NVIDIA DLSS and AMD FSR support disabled.

Set `FORZA_NATIVE_RENDERER=velocity` in the game's environment to replace the
recognized motion draw using an owned blur attachment and compatibility copy.
Set it to `velocity-direct` to write directly into the existing attachment,
removing that copy and the intermediate blur allocation. Set it to
`velocity-resolve` to also bypass that pass's EDRAM dump, as described below.
Set it to `validate` to execute the native pass before
the original draw for GPU comparisons. With the variable unset, the original
renderer runs. These are developer options, not a launcher setting or a user
performance recommendation.

The bridge checks the shader pair, primitive, constants, blend/depth/stencil
state, full viewport/scissor and attachment format/sample count. It leaves
unsupported states, active occlusion queries, fractional rasters and unavailable
frame slots on the original path. Three native slots per extent and at most four
extents are retained until shutdown; extent changes never destroy in-flight
resources. Recording uses an immutable packet on the submission thread. Shutdown
drains recording and GPU work before releasing the pass. The direct variant
borrows a storage-compatible RGBA8 view from the render-target owner. The owner
transitions it to GENERAL before dispatch and restores its tracked usage
afterward. The pass never allocates, destroys or transitions that borrowed image,
and its shader touches only the active rectangle. The `velocity-direct` resolve
consumers still use the ordinary EDRAM path.

The borrowed-target tests run at 73x41 and 2560x1440, including odd workgroups,
mirrored sampling, singular/NaN transforms, frame-slot reuse and rejected
incompatible dispatch metadata. Padding in a larger target is compared byte for
byte after every dispatch. All three tests pass with synchronization validation.
A moving-race fixture also matches the original GPU blur within one quantized
step and its car mask exactly across three slot uses, including a reused slot.
The direct runtime variant completed a full native-1440p AI run with threaded
submission and Khronos synchronization validation, with no Vulkan errors.
Those instrumentation-heavy frame rates are not performance evidence.
A subsequent moving-gameplay capture contains four native dispatches writing
the existing RGBA8 target directly. Each is followed by the ordinary resolve
consumer, with no intervening image copy and no intermediate native blur image.
The recovered final output is 2560x1440, including the HUD, minimap and car labels.

## Native RGBA8 resolve

Patch 0081 adds `FORZA_NATIVE_RENDERER=velocity-resolve`. After the recognized
native motion pass, one compute dispatch writes its RGBA8 image directly into
the existing tiled/scaled texture buffer. This replaces the attachment-to-EDRAM
dump and EDRAM-to-texture-buffer resolve. The ordinary texture upload, CPU
readback contract and subsequent attachment clear remain in place. Other
resolves are unchanged. This is not a direct texture-view alias: the source
attachment is cleared immediately after the resolve, so its view cannot safely
serve later consumers.

`ResolveParameters` describes the source rectangle, destination guest position,
pitch, scale, endian conversion and R/B swap. Each invocation writes a complete
16-byte quad, avoiding read-modify-write races between scaled pixels. The mapping
is derived from the SDK's 32-bit tiled texture layout under its included BSD
license. The runtime accepts only a full, immediately preceding native RGBA8
output, single-sample unsigned normalized RGBA8 destinations, integer scales and
matching guest base/pitch. Format conversion, exponent bias, half-pixel fill,
array destinations, MSAA and unsupported layouts use the original resolve.
Intervening draws and cache resets invalidate the candidate. Pipeline creation
and range validation happen before recording resource transitions, retaining
the original fallback on failure.

The render-target owner tracks source layout/visibility. Destination storage
descriptors use the existing submission retirement rules; scaled/shared-memory
range tracking orders later uploads and readbacks. The source is not retained
past its existing owner's lifetime. Pipeline destruction follows the existing
recording/GPU drain. The mode remains disabled by default and builds without
NVIDIA libraries.

The three CPU tests and two GPU tests pass with Khronos synchronization
validation. Resolve fixtures cover 49 combinations of integer scales, all six
endian modes, R/B swaps, source/destination offsets and 2560x1440 output. They
compare every written byte and untouched padding. A separate local fixture from
the original game's GPU resolve matched all 921,600 pixel words exactly across
three GPU executions, including the padded destination buffer. These private
fixtures are not in the repository.

A complete native-1440p AI race with threaded submission replaced over 1,300
resolves and completed its measurement with zero Vulkan validation errors.
The harness terminated the child after saving the report. A separate capture
contains four consecutive native-image-to-buffer resolves: each bypasses the
EDRAM buffer and is followed by the expected clear and texture upload. The final
2560x1440 output retains the HUD, minimap and car labels. Instrumented frame
rates are not performance evidence. A separate uninstrumented run completed at
69.55 FPS average with no stalls over 50 ms, as listed above. The same foreground
and AI-variation limitations apply; this is not evidence of a speedup.

## Resolve from tracked attachment ownership

Patch 0082 adds the independent `FORZA_NATIVE_RENDERER=rgba8-resolve` mode. It
selects the source from the SDK's render-target ownership map rather than the
most recent motion draw. The complete requested tile rectangle must belong to
one RGBA8, single-sample target with the same base and pitch. Mixed owners,
wrapped spans, offset/reinterpreted sources and the unsupported conversions
listed above retain the original resolve. This mode leaves the original motion
draw in place, so resolve behavior can be tested independently of its native
replacement. It does not replace geometry or allocate native world targets.

At native 1440p, a complete AI race with threaded submission and Khronos
synchronization validation completed over 3,300 native resolves without Vulkan
errors. The harness terminated the test child after its report was saved. A
separate moving capture contains eight native image-to-buffer dispatches over
four game frames, covering two consecutive eligible passes per frame. Their
push constants identify the intended RGBA8 source, layout and buffer range;
no EDRAM buffer participates in those dispatches. The recovered 2560x1440 output
retains the original HUD, labels and minimap. A second original-GPU fixture
matched all 921,600 words exactly through three executions, with padding
preserved and no synchronization errors. Both standalone SDK fallback and
canonical native build modes compile.

A captured full-scene RGBA8 resolve uses the SDK's half-pixel edge-fill rule.
A direct texel copy differs there, so that case is deliberately still rejected;
its original GPU buffer is retained locally for the next implementation. These
results establish only the two supported resolve passes, not complete native
world rendering or a performance improvement.

## Build the development tools

No SDK, game files or NVIDIA runtime is needed for the planner tests:

```sh
cmake -S tools/native_renderer -B build/native-renderer-tools
cmake --build build/native-renderer-tools
ctest --test-dir build/native-renderer-tools --output-on-failure
```

The optional GPU test needs Vulkan headers/loader and `glslangValidator` from a
Vulkan SDK. Its SPIR-V is generated in the build directory, never committed:

```sh
cmake -S tools/native_renderer -B build/native-renderer-gpu -DNATIVE_RENDERER_GPU_TESTS=ON
cmake --build build/native-renderer-gpu
ctest --test-dir build/native-renderer-gpu --output-on-failure
```

Add `-DNATIVE_RENDERER_VULKAN_VALIDATION=ON` to require the Khronos validation
layer and synchronization checks. Set `VK_LAYER_PATH` if it is not installed in
the loader's standard search location. A requested validation run fails when the
layer cannot start. An ordinary GPU run returns CTest's skip code 77 if there is
no compatible device. CPU tests remain available without a Vulkan SDK or GPU.

To also build the capture exporter, supply `RENDERDOC_INCLUDE_DIR` pointing at
the official `renderdoc/api/replay` headers and `RENDERDOC_LIBRARY` pointing at
the matching replay library/import library. Keep RenderDoc's shared library on
the runtime search path. The tool uses C++17 because the tested RenderDoc 1.46
headers contain aggregates that are not compatible with C++20 initialization.

```sh
native_renderer_capture gameplay.rdc build/native-renderer/capture 34 14966
python tools/native_renderer_report.py build/native-renderer/capture
```

This writes `graph.json`, `images.tsv`, `buffers.tsv`, `summary.json` and optional
`event-<id>.json` records. All generated output belongs under `build/`, not in
commits. Shader/descriptor IDs belong only to that capture and are not stable
game shader identifiers.

## First capture validation

A local NVIDIA Vulkan gameplay capture produced 11,364 resource-using operations
over 579 resources: 9,675 draws, 1,014 dispatches, 314 copies, 355 clears and six
other operations. The report verified 64,756 strictly preceding dependencies.
The inventory found 198 small-draw inspection candidates. These are capture
totals, not an assertion that they all belong to one guest frame.

Three reads follow a discard under conservative read/write storage-descriptor
classification. A write-only compute shader can be reported as read/write by
the capture API, so these are inspection points, not confirmed rendering bugs.

Two inspected world draws used the same captured vertex/pixel shader modules
but different viewport heights (928 and 1440), both clipped to a 512-pixel-high
scissor. That is evidence of a tiled/offset rendering contract, not sufficient
proof that either draw can be removed. The export now records viewport, scissor,
shader-module and descriptor metadata for selected events without dumping their
contents. Graphics events exclude stale compute bindings, and vice versa.

The standalone tests cover scene snapshots surviving later attachment writes,
multiple outstanding readers, in-place updates, discard, invalid resource IDs,
aliased views, feedback and dependency ordering. Assertions remain active in
Release builds.

## Next execution milestone

1. Compare warmed, uninstrumented performance at native 1440p and exercise live
   scene resolution changes. Add explicit contracts for fractional scene inputs;
   keep unsupported states on the original path until their output is validated.
2. Map the pass's guest texture/resolve identities to explicitly owned native
   images. Preserve immutable snapshots for consumers that outlive a source write.
3. Extend the validated RGBA8 resolve beyond the recognized motion output, using
   actual render-target ownership and complete source coverage. Add other
   formats and MSAA only with original-GPU comparisons. Longer-term direct
   texture-view aliases need independent snapshots and consumer-fence lifetimes:
   the observed resolve clears its source attachment immediately afterward, so
   retaining that view would be invalid. Keep the option off by default.
4. Check reflections, labels/HUD, cutscenes and menus before extending the path to
   more of the world renderer.

A complete native world renderer remains the goal. The first pass's gameplay
validation establishes a limited replacement with the existing native HUD; it
does not establish native geometry rendering or a performance improvement.
