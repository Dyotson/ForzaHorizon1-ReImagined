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

The first implementation in this branch is **development infrastructure**, not
an enabled replacement renderer:

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

The planner currently works at whole-resource granularity. Layout transitions,
queue ownership, subresources, native allocation and guest EDRAM alias mapping
are not implemented. Captured resources import their initial replay contents;
this does not establish how a native frontend will populate them. Exported graphs
therefore explicitly say `execution_ready: false`.

## Build the development tools

No SDK, game files or NVIDIA runtime is needed for the planner tests:

```sh
cmake -S tools/native_renderer -B build/native-renderer-tools
cmake --build build/native-renderer-tools
ctest --test-dir build/native-renderer-tools --output-on-failure
```

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

1. Identify a world pass with its complete inputs and viewport/scissor contract.
   Check repeated draws across tiles against reflection/camera changes.
2. Map that pass's guest texture/resolve identities to explicitly owned native
   images. Preserve immutable snapshots for consumers that outlive a source write.
3. Implement its layout and queue transitions, format interpretation and CPU
   readback fallback, behind an option that defaults off.
4. Execute the replacement pass, compare actual output at native 1440p, then run
   the AI benchmark with warmed shaders. Check reflections, labels/HUD, cutscenes
   and menus before extending the path.

A complete native world renderer remains the goal; the current tooling is not
presented as that renderer or as a performance improvement.
