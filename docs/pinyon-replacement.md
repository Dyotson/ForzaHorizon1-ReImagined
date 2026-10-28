# Pinyon Shift implementation replacement

This is an implementation and provenance record, not a declaration that the
project is free of Pinyon Shift code. The BSD notice and packaging remain because
the HDR implementation, SDK dependency and possible reused source still require
review and replacement. Three obsolete archive copies have been removed. The BSD-3-Clause terms require preserving the copyright,
conditions and disclaimer in source distributions and reproducing them with
binary distributions; they do not require relicensing this project's own code.

## Repository inventory, 2026-10-27

| Repository | Audited main commit | Tracked paths | Local branch heads |
| --- | --- | ---: | ---: |
| ForzaHorizon1-ReImagined | `52d5784af194328802a98d43e0c633265c922645` | 1038 | 180 |
| ReIGlue | `7d17ea51a51861cfef4244afc06a908bf1fd74a2` | 1409 | 4 |
| forza-live | `526b8bb57906` | 57 | 3 |
| website | `9e73f29acd97` | 29 | 1 |
| .github | `f901dcdfd21a` | 1 | 1 |

The organization lists these five repositories. Searches cover tracked files,
submodule entries and every local branch head for `pinyon`, `shiftglue` and
`arcanite24`. The last three repositories have no matches. A negative text search
does not establish independent authorship; provenance and diffs are also needed.
The game's server submodule points to this organization's forza-live repository.
Old local feature branches still contain the inherited implementation. This work
does not rewrite published history or overwrite anyone's active checkout.

## Replaced here

| File | Replacement and evidence |
| --- | --- |
| `forza-recomp/src/platform/presentation_policy.h` | New countdown state machine. World evidence can acquire for two presentations, retain for thirty; map evidence suppresses three presentations. It accepts evidence before frame zero and resets on initialization. |
| `forza-recomp/src/platform/ultrawide.cpp` | New synchronized session owns dimensions, projection and state. Host callbacks run outside the lock. Existing cvar names remain the configuration interface. |
| `forza-recomp/src/platform/ultrawide_math.h` | Geometry derived from a 720-line presentation canvas and a bounded HUD rectangle. Nonfinite hot-reload values select neutral defaults. |
| `forza-recomp/src/hooks/ultrawide_projection.cpp` | New guest getter adapter uses the established viewport fields and caller addresses, and preserves the VR eye-aspect override. Addresses and memory layout are interface facts. |
| `forza-recomp/tests/presentation_policy_test.cpp` | Acquisition/expiry, map priority in both callback orders, background rendering, disable and reset. |
| `forza-recomp/tests/ultrawide_math_test.cpp` | Existing pixel-shape, safe-area, anchor and backing-height checks; nonfinite input cases added. |

These replacements were written from the existing interfaces and documented
behavior. This is not a claim of a segregated clean-room process: the previous
implementations were inspected during the provenance audit.

## Removed archives and remaining covered or unresolved material

| Location | Status |
| --- | --- |
| `patches/rexglue-sdk/0063-native-ultrawide-and-hud.patch` | Removed from the obsolete archive. It is not consumed by current builds; historical Git revisions retain the original notices. |
| `patches/rexglue-sdk/0082-spirv-apple-color-shader-contraction.patch` | Removed from the obsolete archive. Live ReIGlue replacement uses a completed-module pass. |
| `patches/rexglue-sdk/0091-hdr-scrgb-presenter.patch` | Removed from the obsolete archive, including its embedded generated SPIR-V. The live SDK still contains remaining covered integration. |
| ReIGlue `src/ui/vulkan/vulkan_presenter.cpp`, `include/rex/ui/vulkan/presenter.h` | Separate SDK PR #5 replaces image ownership, format selection, composition/output pass construction and coverage/output recording. Stable interface declarations, cache invalidation and shutdown calls remain for final provenance review. The game pin still uses the prior implementation. |
| ReIGlue `src/ui/shaders/hdr_output.ps.glsl`, `hdr_ui_mask.ps.glsl` and their `vulkan_spirv/*` headers | Replacement shaders and regenerated binaries exist on the separate ReIGlue branch; the pinned SDK still carries the adaptation. |
| ReIGlue `src/graphics/shaders/hdr_hud_mask.cs.glsl` and generated header | A shared-tile replacement exists on the separate ReIGlue branch; the pinned SDK still carries the adaptation. |
| ReIGlue Vulkan command processor and render-target cache HDR paths | SDK PR #5 replaces compute construction, descriptor recording, capture decisions and image allocations, including reused VR allocation/sharpening helpers. Existing frame fences and render-target transfer integration remain. |
| `forza-recomp/src/hooks/ai_catchup.cpp`, `src/platform/ai_catchup.h` | Research citation retained. Compared with reference `9331e453`: this implementation hooks config fills, exempts online modes and uses a bounded trace cache; upstream hooks spawn registers, modifies additional cheat fields and traces with a map. Shared field offsets are game-layout facts. No imported upstream function was identified. |
| `docs/launcher.md`, `launcher/forza_launcher/save_editor.py`, `saves.py` | Research citation retained. Initial commit `ad75b8b` records the independent save manager/editor. Its bounded Python parser differs from reference C++ profile parsing; save management also differs from `tools/pinyon_saves.py`. The only identical significant lines found in the compared save managers were two standard-library imports. |
| macOS tuning/performance comparison documents | Historical measurements and citations retained. They are not runtime dependencies. |

The source commits named by the HDR documentation are ShiftGlue `36275a6`,
`2c82d4d` and `159464d`; its macOS reference is `524c6fb`. The local reference
checkout preserves those revisions. The Pinyon game repository carries the BSD
3-Clause notice; ShiftGlue retains ReXGlue/Xenia BSD notices as well. All remain
intact where their material remains.

The experimental ReGPU checkout is pinned at
`bdd133abb56824eef2cd59d2864f2f051c59d207`. Its actual license is MIT, copyright
2026 Scott. The active backend integration (`feature/regpu-backend`, `21b6372`
at audit start) preserves ReGPU/FidelityFX notices and the ReXGlue/Xenia BSD
notice for its adapted float24 conversion. It is not evidence of a Pinyon
dependency. The backend owner reported no Pinyon imports; their active edits
were left untouched.

## Validation and limits

Windows clang 23.1.3, C++20, `-Wall -Wextra -Werror`: presentation policy and
ultrawide geometry tests pass. Both changed production translation units pass
syntax checks against the existing r2 SDK headers. No game run, GPU image
comparison, full link or macOS runtime validation has been performed for this
replacement. The SDK's own contract tests and syntax checks are recorded in its
corresponding replacement branch. No game save or qualified SDK binary changed.

## Replacement checkpoints

The game branch is PR #493. The companion ReIGlue branch is PR #5, source
checkpoint `7bb18e3`. The SDK has eight passing standalone contract tests:
HUD geometry, shader contraction, HDR color, parameter ABI, format selection,
generated shader bindings, scene capture policy and injected compute-allocation
failures. Both updated production translation units pass Windows syntax checks.
Earlier checkpoints linked and passed DLL loader/ABI checks; this later source
checkpoint still needs its own link and hardware validation.

The source comparison is evidence about particular implementations, not an
inference from absence of a project name. Research references remain useful and
are not removed to conceal provenance. Historical branches, published binaries
and the current SDK pin retain their original source and required notices.
The active ReGPU branch also inherits its game's existing presentation code;
its newly added backend was the part checked for additional Pinyon imports.
