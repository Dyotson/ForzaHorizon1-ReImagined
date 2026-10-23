# Original FH1 native menus

The native pause menu uses repository-authored pages, layout, navigation and
input handling. It requires the player's normal FH1 TU4 installation. No FH2
executable, extracted scene, texture, translation or generated include is used
by this build. The older FH2 experiment is documented in
[horizon2-menu-port.md](horizon2-menu-port.md) for reference. Its translated
runtime adapters and build configuration have been retired.

## Structure

The combined WIP includes the native tuning workspace described in
[car-tuning.md](car-tuning.md). Cars exposes `car_tuning` through its native
availability gate, alongside Change Car, Garage and Autoshow. Tuning returns
through the pause page visibility API, preserving the selected Cars action.
Both consumers use the same TU4 widget helper; its optional donor layer offset
defaults to the existing pause behavior. Pause tabs now use the reusable
`fh1_menu_components::TabBar` renderer while the pause controller retains input
and capability ownership.

The Cars composition uses a larger full-width photo with five specification
columns underneath. Both menus bind real controller icons through the shared
native footer model API. The resource manifest now prepares the pause and tuning
aliases together; both runtime options remain independently disabled by default
in the launcher. Named setups support Save, Load, Rename, Delete, Export and
Import. Community is an unavailable Coming soon page with no action dispatch.

A combined 1280x720 Vulkan copied-profile session verified Home, the selectable
Tune Car tile, entry into the actual tuning workspace, in-game naming and a
saved Setup 1 entry in Load, file export, the unavailable Community page, Back
to Cars with Tune Car focused, and resume. All screenshots came from that same
combined binary and copied profile. No tuning values were applied in this pass.
The tuning worker's separate tests cover file import and persistence/readback;
those are not claimed as repeated combined-session checks. Header class/PI
binding uses the current loaded car's native badge controller; its manufacturer
logo comes from the database's standard IconPath resource. The legacy light
filename is not used because the corresponding texture is absent in FH1.
Both tuning and library use the same header identity. Platform, fallback and extended lifetime checks remain
limited as detailed below.

The shared tab bar uses compact spacing, upright native labels and angled FH1
paper. Selection commits immediately while the active paper moves for150ms;
interrupted motion continues from the displayed location, wrapping snaps, and
large elapsed times settle immediately. This visual state belongs to the shared
component and is preserved through tuning/library view snapshots. The pause
header retains its native player/car/credits bindings over one continuous paper
strip. Only the dim backdrop bleeds outside the safe area, sized from the output
viewport so the donor texture's transparent perimeter is outside the frame.

The shared UI polish was checked at1280x720 in a copied offline profile:
Home/Cars header and slanted tabs, Tune Car entry, tuning/library manufacturer
logo and current B437 badge for the Lancer, named Setup1 under Load, shoulder
switches, Back and resume. Native screenshot sequences capture the active-paper
motion on both pause and library tabs; no synthetic interpolation was added.
The current class is cached until its native value changes, avoiding redundant
texture/text rebinding every second. This pass did not Apply any tuning values.

| Layer | Responsibility | Source |
| --- | --- | --- |
| Model/controller | Stable page and tile IDs, spatial navigation, per-page focus, action intent, input repeat and child suspension | `forza-recomp/src/platform/menu_model.h` |
| Default pages | Home, Cars and Settings, semantic actions, 1280×720 design rectangles and live availability | `forza-recomp/src/platform/native_pause_pages.h` |
| Native rendering | FH1 presentation lifetime, isolated visual components, text/material properties and native data bindings | `forza-recomp/src/platform/fh1_menu_widgets.h`, `forza-recomp/src/hooks/native_pause_scene.cpp` |
| Asset availability | Bounded archive-directory preflight before native scene requests | `forza-recomp/src/platform/native_pause_assets.h` |
| Game actions | Native FH1 child screens, offline garage, festival travel and recovery | `forza-recomp/src/platform/fh1_pause_actions.h` and the `fh1_*`/`offline_cars` adapters |
| Resource preparation | Verified FH1 donor aliases and the existing offline car reload flow | `tools/native_menu_scene.py`, `tools/native_menu/fh1_donors.json` |

The controller has no game, renderer or SDK dependencies. A page owns a vector
of tiles; each tile has an ID, label, rectangle, action, visibility and enabled
state. An empty action creates an informational panel. Updating these values
preserves valid selection by ID. Directional navigation uses the rectangles,
so it works with uneven tiles without a table of hand-coded neighbours.

The native adapter rechecks FH1 action gates before dispatch. Opening a child
suspends the controller and hides the parent presentation; returning requires
held buttons to be released before they can activate another action. FH1 keeps
ownership of its action model, child screens and callbacks.

## Preparing and building

TU4 builds compile the native implementation by default. Set
`FORZA_NATIVE_PAUSE=OFF` to exclude it; base-game builds keep the stock pause
menu. Remove old `FORZA_FH2_NAVIGATION_DIR` and `FORZA_FH2_GRID_EVENTS_DIR`
cache values when reusing a build directory. They are diagnosed before SDK
discovery or code generation.

Enable **Advanced → Menu → Native pause menu (experimental)** in the launcher,
then restart the game. The equivalent runtime switch is
`--forza_native_pause=true`. It remains opt-in while the new native composition
is undergoing broader platform and lifecycle validation.

LB/RB switch between Home, Cars and Settings; directional input selects an
available tile, and A opens it. B/Start resume driving. Left-stick click requests
the native Reset Car Position action when it is available. Returning from a
child restores the previous page and focused tile, even when FH1 recreates the
pause screen. Resuming and then opening pause again starts on Home.

The launcher prepares resources from its freshly applied FH1 TU4 update during
installation. Unsupported or modified input leaves the stock update available
and logs why native menu preparation was unavailable. A complete update is
published only after preparation and validation succeed. The original update
and the player's input files remain unchanged.

Before requesting donors, the runtime checks the mounted update archive for all
33 required, nonempty scene members. Missing, incomplete or unsupported archives
keep the stock pause immediately. This is necessary because FH1's native scene
request can stall on a missing resource before a UI timeout can run. The check
reads ZIP directory metadata and accepts native method-21 payloads without
decoding them; the preparation tool remains responsible for integrity checks.

For a direct developer build:

```sh
python tools/native_menu_scene.py /path/to/original/fh1-tu4 --output /path/to/new/private/menu-update
```

Point `--update_data_root` at that generated directory. The tool verifies the
supported original TU4 archive SHA-256 and executable title/version. It copies
complete FH1 scene triplets into distinct resource aliases without decoding or
changing their geometry. It also patches the owner's FH1 pause XML for the
offline car reload path, using the repository's Python LZX decoder. All output
contains game-derived data and stays in ignored private directories.

This is resource preparation, not a general scene-file serializer. Rendering
uses isolated native components from those donor presentations. No serialized
game scene is committed to the repository.

## Extending the menus

1. Add or modify `Page`/`Tile` descriptors. Keep IDs stable when changing their
   order or labels so focus survives dynamic refreshes.
2. Register an action and its availability/dispatch behavior in the native
   adapter. Layout must never override the game's permission checks.
3. Bind additional data separately from layout. The map adapter accepts an
   image rectangle; the car card keeps native names, units and performance data.
4. Validate the renderer's capacity before transferring ownership from stock
   controls. The first FH1 backend has six tile slots and three tab slots; the
   controller itself supports arbitrary counts. Expanding this backend requires
   additional donor aliases, matching slot/tab definitions, and memory/render
   validation. An unsupported layout must retain the stock menu.

New renderers can consume the same controller without carrying over FH1 guest
addresses. Changes to input repeat, focus memory or navigation therefore remain
independent of scene composition and game actions.

## Validation

The SDK-free model tests exercise arbitrary page counts, uneven geometry,
availability changes, spatial reachability of all default pages, stable focus,
child return, opposing inputs and repeat timing. Preparation tests use authored
synthetic data, including compressed XML; they cover deterministic output,
archive offsets, source preservation and malformed input. Configuration and
disabled-stub tests require neither game files nor translated includes.
Archive preflight tests cover every missing donor member, malformed directories,
empty/encrypted entries, native method-21 metadata and ZIP comments without game
files or a decompressor.

```sh
python -m unittest discover -s tools -p 'test_native*.py' -v
python -m unittest discover -s launcher -p 'test_native_menu*.py' -v
cmake -S forza-recomp/tests/menu-model -B build/menu-model
cmake --build build/menu-model --parallel 2
ctest --test-dir build/menu-model --output-on-failure
cmake -S forza-recomp/tests/pause-stubs -B build/pause-stubs
cmake --build build/pause-stubs --parallel 2
ctest --test-dir build/pause-stubs --output-on-failure
```

The initial private preparation audit preserved 917 original archive entries
byte-for-byte, intentionally replaced only the active `pause.xml` entry, and
verified all 36 alias payloads against their FH1 donors. All three executable
copies matched the source.

On 2026-10-22, a private Windows TU4 build compiled and linked all twelve affected
runtime translation units against the actual SDK with no FH2 inputs or flags.
Base-game and disabled-TU4 hook compilation also passed. The game ran offline on
copied saves at 1280×720, Vulkan, native render scale, on an NVIDIA GeForce
RTX 5060 Ti with driver 617.14. Visual checks covered all three pages, native
current-car data, player/credits/progress, map preview, and the corrected
Difficulty title. World Map, Change Car, Graphics, Audio and Difficulty opened
successfully; child returns, resume, and reopening pause behaved as described
above. No profile settings were changed in these menu checks.

The final binary also passed two archive-availability cases: opt-in with pristine
TU4 assets immediately retained usable stock pause and resumed; the staged
archive activated Home/Cars and resumed. The preflight was added after the first
missing-file run exposed a stall inside FH1's loader before the UI timeout could
execute. The corrected Difficulty heading was verified in a separate run.

The short session included three pause openings and several child screens. Whole
process private memory ranged from 2.97 to 2.99 GB, including screenshot and
resource warming; these observations do not establish the absence of leaks or a
stable long-session memory cost. Linux, other GPUs, races, Rally, multiplayer,
paid travel and extended repeated-open testing remain unvalidated.

Integration with main `04b7d31` preserved the current controls/remapping and
gamertag behavior. Fourteen SDK-free model/archive CTest cases, 59 focused
launcher/menu/bindings/gamertag tests and syntax checks of four enabled TU4 menu
hooks passed after that merge. The game captures above predate this integration;
no post-merge game session or full rebuilt SDK/game validation was performed.

### Current-main conflict and packaging repair (2026-10-22)

Integrated main `7f9a716` in an isolated checkout of combined checkpoint `3711096`.
The resolution retains both benchmark and pause/tuning includes, all catalog and
menu packaging tools, and current-main ASCII gamertag filtering alongside menu
setting-length/coercion behavior. Active menu/tuning worktrees were not changed.

Fixed Linux release tar naming: the native-resource loop had overwritten the
package name with `native_menu/fh1_donors.json`, so `--tar` attempted to write a
nonexistent nested archive path. A synthetic Linux-package regression checks the
release filename and all nested preparation resources.

Windows validation after integration: 618 launcher tests passed with 37 skips;
20 distribution/install tests and 23 resource-staging tests passed. All 14 menu
model/archive, three tuning/component/setup and two disc/TU4 disabled-hook CTests
passed. The actual graphics, pause, scene, map and two tuning hook sources compile
with TU4/native-pause enabled against the installed Windows SDK headers. No game
or GPU session, full game link, shared SDK installation or gameplay claim was made
in this conflict-repair pass. The draft/runtime-opt-in gates above remain.

The first CI rerun exposed a Windows-only test-path mismatch: the runner's TEMP
path used `RUNNER~1`, while the builder deliberately resolved the workspace to its
long spelling. The preparation mock now compares resolved paths, retaining its
workspace/TU containment assertions. A lexical-alias regression exercises that
normalization on every host; the full Windows suite was rerun successfully.
