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
