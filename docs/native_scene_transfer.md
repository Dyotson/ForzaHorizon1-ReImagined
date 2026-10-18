# Native world-color scene transfer

`FORZA_NATIVE_RENDERER=world-color-transfer` adds an opt-in scene transfer to
the authored [world-color pass](native_world_color.md). `world-color` and the
other diagnostic modes keep their existing behavior. The default is unchanged.

## Transfer contract

The captured scene chain dumps an R10 attachment to emulated EDRAM, resolves it
to guest-addressed tiled memory, then untiles that memory into the texture read
by world-color composition. This resolve also applies exponent bias -2: treating
it as an unconverted image alias produces the wrong exposure and rounding.

The new compute dispatch reads the owned attachment, reconstructs its packed
channels and applies the original conversion. It writes both the canonical
tiled backing and an independent R32_UINT snapshot containing the converted R10
bits. The first matching texture load copies those bits into its existing R10
image with `vkCmdCopyImage`, bypassing its untiling dispatch. That load also
records the complete consumer key. Later producers reacquire that cached texture
and write the converted bits directly through its existing R32_UINT storage view.
This bypasses both the snapshot copy and untiling for that exact consumer.
Conversion finishes before the source attachment can be cleared or reused.

This removes the supported scene's EDRAM dump and texture reconstruction. It
still writes canonical tiled memory. Initial or unsupported consumers retain the
snapshot-copy or ordinary upload path. CPU readbacks,
other texture interpretations and unsupported consumers retain valid backing.
The already-direct HUD handoff, original geometry and reflection passes remain
in use; this is not a complete native renderer or a Metal backend.

## Selection, ownership and fallback

The producer requires one complete, non-wrapping, tracked R10 attachment with
one sample, unsigned R10 output, no channel swap, offset or array, the expected
pitch, integer equal scales 1 or 2 and exponent bias 0 or -2. Fractional raster
states and unsupported endian modes retain the original path. The unscaled
scene contract is 1280x720, including its original half-pixel edge-fill rule.
Scale 2 produces a native 2560x1440 scene; HUD composition stays at output size.
Other R10 sizes, pitches and offsets are rejected before walking render-target
ownership. The selected diagnostic mode is read once, as on the other native
resolve paths. The final adapter retains its complete contract checks.

Consumers must match address, byte extent, dimensions, pitch, endian and scale,
and use a tiled, unsigned, single-mip R10 2D texture without an array. Merely
reusing the guest address does not establish identity. Overlapping CPU/GPU
memory writes invalidate published snapshots through the shared-memory global
watch. Publication follows the producer's own invalidation while holding the
same recursive critical region, preventing a writer from entering that gap.

Eight slots own their images and views, with an actual allocation budget of
128 MiB. Both producer and last copy reader extend a slot's retirement fence;
only waited submissions allow reuse. Invalidation does not free an in-flight
image. Allocation failure, missing descriptors, budget exhaustion or any
unsupported state returns to the original path. No device-idle wait is added.
The direct path remembers a key rather than an image pointer across producers,
so eviction cannot leave a stale pointer. Each producer reacquires and marks the
normal cached image as used. Its existing usage barriers order previous shader
reads before the compute write and subsequent sampling after it. Sampling views
stay attached to the same cache-owned image; no attachment alias is introduced.
After canonical backing invalidation, only the populated texture is marked
current and watched, under the same recursive invalidation lock. Other keys and
later overlapping writes continue to use the normal cache invalidation rules.
Missing storage views or descriptors retain the independent-snapshot fallback.

The texture-cache owner drains recording/GPU work before destroying resources
and unregisters the memory watch before releasing the state.

SDK patches 0088 and 0089 connect these rules to Vulkan. Canonical headers and shader
sources are supplied through `REXGLUE_FH1_NATIVE_SOURCE_DIR`; builds without
those sources keep the standalone fallback. The public Vulkan function-table
ABI is unchanged.

## Validation

- A private original-GPU fixture takes the source attachment and the original
  resolved backing from the same replay state. All 3,686,400 converted 1440p
  snapshot pixels match exactly, and the canonical backing has zero mismatched
  words. Synchronization validation reports no errors. Game images, shaders,
  captures and fixture bytes are not committed.
- Synthetic GPU coverage includes 340 R10 cases: gain 1 and 0.25, source
  swizzles, endian variants, unequal scales, half-pixel fill, destination
  offsets/padding and full 1440p images. The test clears the source before
  consuming the snapshot, then checks both the raw-copy consumer and backing.
  The live bridge deliberately accepts a narrower state set than the shader.
- Another 340 cases write directly through an R32_UINT view of a mutable R10
  image, without the intermediate image copy. They clear the original source
  before reading the result and check exact converted bits and tiled backing.
- CPU tests check identity, overlapping and adjacent writes, stale versions,
  monotonic submissions and protection of invalidated in-flight resources.
- All 16 standalone tests pass: seven CPU and nine GPU tests, with Khronos
  synchronization validation enabled for the GPU suite. The CPU-only build
  passes all seven tests without Vulkan. Canonical and standalone-fallback
  Windows SDK builds pass, and the full patch series applies to its pinned SDK.
- A copied-save race executes the R10 producer, snapshot consumer and authored
  world-color pass, logging both scene and HUD as 2560x1440. Its final presenter
  screenshot shows the minimap, labels and car reflections. No Vulkan validation
  errors are reported in that session. Screenshots stay outside the repo.

These checks establish the captured contract and tested race, not every scene
or render scale. Keep this mode disabled by default pending broader coverage.

Fresh baseline and native presenter captures both show the full native HUD and
car/environment reflections at 2560x1440. They are nearby race moments, not a
frame-identical visual comparison. Neither capture session reports Vulkan
validation or synchronization errors. Both log one `native_hud_seed` rejected
draw during startup; this also occurs with the transfer disabled, and the
gameplay HUD subsequently renders. That separate startup issue is unresolved.

A separate instrumented test host, using the same GPU DLL as the timed runs,
restored an offline pre-story checkpoint and requested the complete `FMV_02`
sequence through the native career state machine. Three distinct video frames
were captured at 2560x1440 output. The pre-movie, movie-entry, movie-exit and
post-movie-completion markers all fired; the map and subsequent car-selection
screen rendered after the transition. No Vulkan validation/synchronization
errors were reported. The movie itself retains its original video resolution;
1440p here describes the presenter output. This does not cover every cutscene.

## Warmed native-1440p comparison

The RTX 5060 Ti 16 GB / i9-13900K machine used the same host, runtime/GPU DLLs,
copied save seed, shader cache and AI race for both modes. Scene and HUD were
2560x1440, with SMAA, no temporal upscaling, vsync, threaded Vulkan submission
and a configured 120 FPS cap. The original scene's multisampling remains active.
Only `FORZA_NATIVE_RENDERER` changed between runs.

One warm-up for each mode is excluded. Four measured runs used baseline /
transfer / transfer / baseline order, with 15 seconds settling and 60 seconds
measured each. Baseline means the already-authored `world-color` pass; this
is an incremental transfer comparison, not an original-renderer comparison.
Validation and RenderDoc layers, presenter readbacks and GPU replays were absent
from the timed runs. Per-second audits found no other FH1 process.

| Run | Scene transfer | Average FPS | 1% low FPS | P99 | Worst | >50 ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | Baseline | 58.99 | 39.63 | 19.92 ms | 36.46 ms | 0 |
| 2 | Native snapshot | 55.32 | 37.10 | 21.16 ms | 36.81 ms | 0 |
| 3 | Native snapshot | 57.84 | 37.82 | 19.15 ms | 37.12 ms | 0 |
| 4 | Baseline | 59.86 | 38.88 | 18.87 ms | 34.97 ms | 0 |
| Pooled | Baseline | 59.42 | 39.09 | 19.45 ms | 36.46 ms | 0 |
| Pooled | Native snapshot | 56.58 | 36.92 | 20.68 ms | 37.12 ms | 0 |

The original native snapshot path is **4.78% slower on average** in this comparison,
with a lower 1% low. This does not establish a performance improvement. It
remains opt-in, and the normal renderer is unchanged. The remaining canonical
writes and image copies need profiling before further optimization; these
end-to-end results do not attribute the regression to one GPU operation. These
numbers precede the direct cached-image output in patch 0089.

Pooled statistics use saved frame intervals: 7,130 baseline intervals over
119.985673 seconds and 6,789 native intervals over 119.983235 seconds. CSV counts
and summed times agree with the reports, and binary hashes match across all
four runs. The 1% low is the reciprocal mean of the slowest ceil(N/100) intervals.

Parsec streaming was active during the measured set, and one desktop inspection
occurred during the first baseline. AI traffic varies, and focus was not
continuously monitored. These are development results for this machine/scene,
not isolated GPU costs or a deterministic hardware ranking. Earlier shared-load
runs and both warm-ups are excluded.
