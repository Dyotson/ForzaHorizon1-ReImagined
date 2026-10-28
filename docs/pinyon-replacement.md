# Pinyon Shift implementation replacement

This is an implementation and provenance record, not a declaration that the
project is free of Pinyon Shift code. The BSD notice and packaging remain because
the HDR implementation, SDK dependency and historical patch copies still require
review and replacement. The BSD-3-Clause terms require preserving the copyright,
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

## Remaining covered or unresolved material

| Location | Status |
| --- | --- |
| `patches/rexglue-sdk/0063-native-ultrawide-and-hud.patch` | Historical source still includes the old HUD/view presentation implementation. ReIGlue has the live implementation. Retain notice until obsolete covered source is removed. |
| `patches/rexglue-sdk/0082-spirv-apple-color-shader-contraction.patch` | Historical copy of the old shader-builder adaptation. Live ReIGlue replacement uses a completed-module pass. |
| `patches/rexglue-sdk/0091-hdr-scrgb-presenter.patch` | Contains adapted HDR presenter and shaders, including generated SPIR-V. Still retained and covered. |
| ReIGlue `src/ui/vulkan/vulkan_presenter.cpp`, `include/rex/ui/vulkan/presenter.h` | HDR stand-in images, output pass, swapchain selection and resource lifecycle derive from ShiftGlue. Not cleared by replacing shader code alone. |
| ReIGlue `src/ui/shaders/hdr_output.ps.glsl`, `hdr_ui_mask.ps.glsl` and their `vulkan_spirv/*` headers | Replacement shaders and regenerated binaries exist on the separate ReIGlue branch; the pinned SDK still carries the adaptation. |
| ReIGlue `src/graphics/shaders/hdr_hud_mask.cs.glsl` and generated header | A shared-tile replacement exists on the separate ReIGlue branch; the pinned SDK still carries the adaptation. |
| ReIGlue Vulkan command processor and render-target cache HDR paths | Scene snapshot and coverage integration require line-level comparison with the source fork. |
| `forza-recomp/src/hooks/ai_catchup.cpp`, `src/platform/ai_catchup.h` | Existing source credits the config-rewrite idea and controller research. Separate factual addresses from any borrowed expression before changing attribution. |
| `docs/launcher.md` save-format reference | Research citation retained. A citation by itself does not mean the save editor copies implementation. Verify file history rather than delete the reference. |
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
