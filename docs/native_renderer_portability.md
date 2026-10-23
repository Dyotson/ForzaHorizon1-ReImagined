# Native renderer portability

## Intended boundary

The native frontend describes scene views, meshes, materials, lights and pass
inputs. Its resource versions and submission lifetimes must not contain Vulkan
handles, Xenos registers, guest addresses or Metal objects. Each backend owns
allocation, binding, barriers, render encoders and submission completion.

The existing game still supplies Xbox rendering commands. During migration an
adapter must recover the semantic inputs from those commands. This adapter is
not the portable renderer. Unsupported commands retain the existing path while
the native path remains an opt-in experiment.

| Layer | Shared contract | Backend responsibility |
| --- | --- | --- |
| Scene | View transforms, mesh ranges, material values, scene/output extents | Uploads and device resource bindings |
| Passes | Color/depth formats, samples, load/store intent, immutable input versions | Vulkan rendering scopes or Metal render/compute encoders |
| Materials | Defined shading operations, parameter packing and output semantics | Shader compilation and resource binding layout |
| Lifetimes | Submission IDs, completed submissions, snapshot ownership | Fences/completion handlers and retirement |
| Presentation | Scene image plus independent output-resolution UI | Swapchain/drawable acquisition and composition |

The current `render_graph.h` is an offline dependency checker, not a runtime
scheduler. The current `GeometryPass` and `VelocityPass` are Vulkan execution
objects. Neither is evidence of an implemented shared scene frontend or Metal
backend. Preserve their tested behavior while introducing that boundary around
a complete pass; do not rename Vulkan wrappers to imply portability.

## First shared material

`vertex_lit.h` expresses a vertex-lit material and its output rules without a
graphics API dependency. `vertex_lit.glsl` separates the authored math from guest
registers and descriptor layouts. The standalone vertex/fragment wrappers use
explicit inputs and an 80-byte material block. CPU tests cover fog, modulation,
opacity, exposure, alpha comparison, sample coverage and gamma behavior. GPU
builds compile these shaders independently of the SDK.

Standalone GPU tests render into independently owned RGBA32F attachments. They
check every pixel against the CPU material contract, including scissor padding,
fog interpolation, output encoding, all alpha comparisons and patterned sample
coverage. The 73x41 fixtures pass at one and four samples, with a separate
2560x1440 single-sample check. All ten standalone tests passed on the RTX 5060 Ti;
the five GPU tests required Khronos synchronization validation and reported no
errors. This is synthetic material validation, not comparison with the original
game's color pass.

Both material stages also translated from Vulkan 1.1 SPIR-V to MSL 2.1 with the
local SPIRV-Cross tool. Generated outputs stay in the volatile build directory.
They have not been compiled by Apple's Metal compiler or executed on a Mac.

The material currently consumes already computed vertex irradiance. It does not
implement the game's probe-lighting vertex stage, render a complete world pass,
or replace a live material. SPIR-V-to-MSL translation can check an intermediate
shader path; only compilation and execution on a Mac can validate a Metal
implementation. Binding layouts belong to backend adapters, not the material
contract.

## Next complete-pass experiment

Select an actual world color pass whose geometry, lighting, depth, multisampling
and downstream consumers can all be accounted for. A simple material is useful
for validating shading, but shader simplicity alone is not a performance case.

1. Measure the original pass's GPU time, CPU submission time, tile replays,
   transfers and attachment footprint in warmed gameplay. Keep instrumentation
   runs separate from end-to-end frame-rate comparisons.
2. Recover immutable mesh and material inputs, including their producers. Give
   the pass explicitly owned scene color/depth targets and a logical full-view
   coordinate system. Remove repeated tile draws only after proving which draws
   are replay of the same scene work.
3. Track every consumer: motion/depth reconstruction, reflections, minimap,
   texture snapshots, CPU readbacks and final scene composition. Retain copies
   when a consumer needs contents that a later draw or clear will overwrite.
   Direct image aliases are safe only when contents and lifetimes match.
4. Execute the complete supported pass once through the Vulkan backend. Preserve
   depth/stencil, sample coverage, blending, output encoding and view orientation.
   Check unsupported states before recording any native mutations. A fallback
   must retain coherent contents for the whole affected pass.
5. Compare original/native color and depth on the GPU with declared tolerances,
   including moving cars, reflections, camera changes, cutscenes and scene-scale
   changes. Verify the HUD, labels and minimap remain at output resolution.
6. Repeat warmed original/native/native/original gameplay measurements with the
   same executable, save, frame cap and settings. Report averages, frame-time
   tails and stalls. Attribute savings to removed work; do not infer them from
   API choice or a successful shader replacement.

The existing rigid-depth experiment measured 69.69 FPS original and 69.02 FPS
native, pooled by frames/time across the four runs documented in
`native_renderer.md`. It establishes no speedup. The experiment retains most
legacy rendering costs and adds mesh snapshots. Its result is not a prediction
of either a complete native renderer or Metal performance.

## Windows backend comparison

A separate dual-backend SDK build tested the existing Vulkan and Direct3D 12
paths on an RTX 5060 Ti 16 GB (driver 617.14) and i9-13900K. Native pass
replacements, DLSS and FSR were disabled. Both APIs used the same public benchmark
host, SDK binaries and copied save seed. To avoid a Vulkan-only scene/HUD path,
both used the original 1280x720 guest coordinates with 2x rasterization, original
game MSAA, 2560x1440 output, no presentation filter, vsync and a configured 120 FPS
cap. Presenter readbacks verified 2560x1440 output with the HUD intact.

Both shader caches were warmed first. Four subsequent runs used Vulkan / D3D12 /
D3D12 / Vulkan order, each with 15 seconds settling and 60 seconds measured.
Statistics below pool the saved frame intervals across the two runs per API;
warm-up runs are excluded. The 1% low is the reciprocal mean of the slowest
ceil(N/100) intervals, as in the benchmark. CSV counts and summed durations agree
with the reports.

| Backend | Average FPS | 1% low FPS | P99 | Worst | Intervals >50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Vulkan | 59.39 | 38.71 | 18.82 ms | 36.61 ms | 0 |
| Direct3D 12 | 51.88 | 22.19 | 22.72 ms | 62.54 ms | 37 |

Individual average rates were 59.38 / 51.53 / 52.22 / 59.39 FPS. Direct3D 12's
average was 12.65% lower in this comparison, and its slow intervals occurred
throughout the race. Its repeat log confirms that stored shaders were loaded
before gameplay. No validation layers, RenderDoc replay or screenshot readbacks
ran during the measured intervals. A process-table audit found no other FH1
processes during any timed run. The game was brought into the foreground before
each run; focus was not continuously monitored. AI traffic varies, so this is a
matched development comparison, not a deterministic hardware ranking.

These results support retaining Vulkan as the first native backend on this
machine. They do not measure the proposed shared scene renderer, establish a
Metal performance result, or attribute the DX12 stalls to a specific cause.
Separate visual-only sessions captured 12 moving 2560x1440 frames per API, with
the minimap, car labels and HUD present. Inspection of those rear-view sequences
did not reproduce the green-reflection or obvious flicker report. This short
view does not close issue #46 or establish parity in other scenes and angles.

This original-MSAA baseline also differs from the earlier native-1440p SMAA
geometry comparison. Source shaders, captures, saves and generated outputs stay
outside the repository.

## Live world-color pass

The next supported live pass is now available independently as
`FORZA_NATIVE_RENDERER=world-color`. It replaces world-color filtering, bloom
combination, exposure, grading and output color conversion with authored Vulkan
shaders. The captured original-GPU output matches within one 10-bit channel
step at 2560x1440. A warmed four-run comparison measured 57.55 FPS original and
59.62 FPS native, with the limitations and full validation recorded in
[`native_world_color.md`](native_world_color.md).

This owns the authored pipeline and its immutable inputs, while retaining the
SDK scene attachments and transfers. It does not yet meet the broader native
world-geometry/resource-ownership milestone above. Neither the measured gain
nor the Vulkan implementation establishes Metal performance.

## Metal validation

After the shared scene pass works on Vulkan, implement equivalent Metal resource
and encoder handling. Validate shader compilation, interpolation, sample masks,
depth conventions, format support and synchronization on actual Apple hardware.
Compare images and timings on the same Mac; Windows measurements cannot establish
a Mac speedup. Vulkan through MoltenVK is a separate portability option, not a
direct Metal backend.

A renderer backend alone does not port the game runtime. macOS/ARM64 builds,
runtime dependencies, windowing, input, audio, file handling and packaging need
their own assessment and tests. DLSS is not a requirement of the shared renderer;
platform upscalers must consume explicitly defined scene depth, motion and
output-resolution UI inputs.

## Current-main conflict repair (2026-10-22)

Integrated main `cb676f7` while retaining native-renderer source paths and all
current wheel regression inputs in the release SDK cache keys. The conflict was
limited to the Linux cache-key line; no native-renderer algorithm changed.
All 106 SDK patches apply in numerical order to pinned SDK `bd833a2`, with unique
numbers, and all seven standalone Windows CPU renderer tests pass. This pass did
not rebuild/install the shared SDK or run GPU/game tests. Earlier full SDK and
runtime results retain their recorded provenance; integrated runtime validation
and the existing performance/portability gates remain outstanding.
