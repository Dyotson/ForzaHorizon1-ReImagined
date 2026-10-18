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
bits. The next matching texture load copies those bits into its existing R10
image with `vkCmdCopyImage`, bypassing its untiling dispatch. This uses a raw
copy between compatible 32-bit formats, without filtering or normalization.
The snapshot is complete before the source attachment can be cleared or reused.

This removes the supported scene's EDRAM dump and texture reconstruction. It
still writes canonical tiled memory and makes one image copy. CPU readbacks,
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
The texture-cache owner drains recording/GPU work before destroying resources
and unregisters the memory watch before releasing the state.

SDK patch 0088 connects these rules to Vulkan. Canonical headers and shader
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
- CPU tests check identity, overlapping and adjacent writes, stale versions,
  monotonic submissions and protection of invalidated in-flight resources.
- All 15 standalone tests pass: seven CPU and eight GPU tests, with Khronos
  synchronization validation enabled for the GPU suite. The CPU-only build
  passes all seven tests without Vulkan. Canonical and standalone-fallback
  Windows SDK builds pass, and the full patch series applies to its pinned SDK.
- A copied-save race executes the R10 producer, snapshot consumer and authored
  world-color pass, logging both scene and HUD as 2560x1440. Its final presenter
  screenshot shows the minimap, labels and car reflections. No Vulkan validation
  errors are reported in that session. Screenshots stay outside the repo.

These checks establish the captured contract and tested race, not every scene
or render scale. Keep this mode disabled by default pending broader coverage.
