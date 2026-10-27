# Pause studio: concrete integration boundary

Status: proposal, not a working native provider. The current experiment changes
the view only. Actual images on 2026-10-27 demonstrate that nearby scenery can
occlude the car during the pan. Moving controls does not solve that defect.

## Required scene production

Produce two scene images from one immutable paused-car snapshot and camera:

1. The ordinary world with the selected car.
2. An owned studio view with the same selected car, pose, camera and lighting,
   a studio background, and no scenery in front of the car.

Blend these scene images before native menu rendering. Both images must use
the same linear color convention, dimensions, AA and exposure. The car is
preserved because it is rendered into both images. Applying opacity to the
finished frame cannot implement this behavior. A thumbnail is not a substitute
for the current render car.

The narrower native route is CUI4PresentationRenderer and its selected-car
render path. The static selected-car gather predicate is proven, but complete
body/glass/shadow rendering and exclusion of the environment are not. An owned
view must use the original deferred factory, registration and retirement path;
neither a shared view's mask nor the live car may be changed to obtain a layer.

## Smallest SDK addition if native UI composition cannot retain the output

Add an optional **scene-image lease and composition service** at the Vulkan
command processor / render-target cache boundary. Keep it separate from the
existing IGraphicsSystem vtable so an old SDK still selects normal rendering.
The existing public interface does not expose this capability. The private
Vulkan command processor already tracks current/completed submissions; that
state belongs in the backend, not in guest addresses interpreted as fences.

The service needs these operations:

| Operation | Required behavior |
| --- | --- |
| Request scene pair | Accept qualified native resolve destinations plus owner, car, pose, camera, lighting, epoch and frame identities. Requesting does not imply readiness. |
| Capture on actual resolve | On the GPU command stream, match the exact destination, extent, format and revision; copy/retain a scene-only image before any UI writes. Resolve addresses alone are not persistent image handles. |
| Compose | Validate both identities and formats; order the fade kernel after both producers and before the first consumer of the scene texture. Preserve alpha and the native UI pass. |
| Retire | Retain images/descriptors through the submission that samples them. Resume invalidates the epoch immediately; GPU resources retire after their last submitted use completes. |
| Fall back | Unsupported backend, missing pair, stale identity, resize, device loss, failed production or mismatched color/AA returns the original scene. Never reuse the previous car/frame. |

Do not acquire a swapchain/present image: it already contains UI. Do not infer
pass identity from draw count, shader hashes, EDRAM addresses or a screenshot.
The game adapter must qualify the native producer and resolve destination; the
backend must establish real image ownership and command-stream ordering.

## Native adapter work still required

- Qualify the original renderer factory parent/service arguments. Its deferred
  wrapper holds the native owner until queued removal; release views before
  releasing that wrapper.
- Establish an owned output texture reference. View record `+60` is a viewport,
  not a texture. Texture requested state `+41=4`, actual state `+40=4`, and an
  underlying graphics pointer are not per-frame completion fences.
- Render one still selected-car-only image through that owned view and prove
  body, glass, reflections, AA and shadows. Then identify its exact resolve
  destination and the ordinary scene's pre-UI consumer.
- Bind the lease service only after those identities are proven. Retain the
  current offline/single-view/exterior/VR gates and cancel on child screens,
  owner/car change, stale world/pause pulses and resume.

This is the minimum missing engine/backend contract. The existing unbound fade
math and shader cannot supply scene production, correct ordering or ownership.

## Native output and context adapter

The bounded output trace identifies a direct native texture path:82729468 takes
a caller-provided texture reference in r5, copies it to a local strong reference,
and submits its final graphics postpass through82CFE8F0 /824728D8 into that
texture's surface. Its local references are released on exit. The existing map
preview demonstrates native UI retaining a separately copied texture reference
through82F38478. This keeps a direct native composition route viable; an SDK
bridge is not yet established as necessary. The renderer also changes its view's
viewport, so it must never be invoked on a shared live view.

The source adapter now observes the original presentation camera update only
while a fresh pause view is published. It checks the concrete camera/core types,
the original selected-car accessor slots and an exact match to the matrix just
published by the pause camera. The native setter may execute on the render
thread after the main-thread reader returns, so a caller-thread scope token is
not treated as proof of scene identity. It rejects ambiguous candidates and clears the
observation on each new frame, cancellation or stale context. This supplies
observed core/camera/selection identities without calling native getters,
allocating anything or changing a guest object. It does not retain those
objects, prove selection maps to the intended render car, or produce an image.
An owned producer must complete those checks before using the observation.

## Acceptance before enabling

Capture actual close-up, pullback and pan frames throughout the fade with the
car unobscured, including the failing location beside Dak's Garage. Verify
Home/Cars/Settings readability, Social fallback, child return, repeated pause,
resize/device failure fallback and normal resume. Keep the option default-off
and the PR draft until those images and lifecycle checks pass.
