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

This branch contains development infrastructure and an executable native compute
pass. It is **not yet an enabled replacement renderer**:

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
  the standalone GPU test; it is not wired into the game's draw stream yet.

The planner currently works at whole-resource granularity. Layout transitions,
queue ownership, subresources, native allocation and guest EDRAM alias mapping
are not implemented. Captured resources import their initial replay contents;
this does not establish how a native frontend will populate them. Exported graphs
therefore explicitly say `execution_ready: false`.

## Native depth and motion pass

The pass accepts a host parameter block containing 25 preceding-frame transforms,
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
before reuse. Destruction waits for the device before releasing resources.

Outputs are R32F depth, RGBA32F motion (XY in output pixels) and RGBA8 blur. The
four-component motion image is intentional during validation: it preserves full
precision without requiring `shaderStorageImageExtendedFormats`, which the
existing device setup does not enable. A supplied function table lets the pass
use the runtime's Vulkan loader. No NVIDIA library is required.

Synthetic fixtures test static and translated cameras, individual car/stencil
IDs, the world mask, capped object IDs, invalid transforms, singular homogeneous
coordinates, blur limits, two orientations and output sizes 73x41 and 2560x1440.
The odd size also exercises partial compute workgroups and readback alignment.
Every output pixel is compared with a CPU reference; closed-form reference tests
independently check expected pixel displacements and depth values.
Additional fixtures exercise a BGRA image-view mapping and an exact 2:1 reduction
in both orientations.

On an RTX 5060 Ti, all three Release tests passed with Khronos synchronization
validation enabled and zero validation errors. At 2560x1440, depth and quantized
blur matched exactly; the largest motion difference was 0.000197 pixels. These
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

The local fixture and replay outputs are volatile files outside the repository.
A new moving AI-race capture currently fails during Vulkan instance creation
under RenderDoc with `ErrorExtensionNotPresent`; the regular game and standalone
GPU test start successfully. Investigate capture compatibility before claiming a
moving-scene comparison. The native pass is still not substituted into the game.

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

1. Complete a moving-camera/AI-race comparison after the neutral-motion replay
   match. Establish tiled/reflection state and fallback conditions before replacing
   the original draw. Neutral-motion parity alone does not establish this.
2. Map the pass's guest texture/resolve identities to explicitly owned native
   images. Preserve immutable snapshots for consumers that outlive a source write.
3. Implement its layout and queue transitions, format interpretation and CPU
   readback fallback, behind an option that defaults off.
4. Execute the replacement pass, compare actual output at native 1440p, then run
   the AI benchmark with warmed shaders. Check reflections, labels/HUD, cutscenes
   and menus before extending the path.

A complete native world renderer remains the goal. The first pass's GPU test
does not establish native game rendering, native-HUD integration or a performance
improvement. Those remain required integration work.
