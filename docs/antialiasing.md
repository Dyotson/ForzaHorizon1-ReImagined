# Scene anti-aliasing

The launcher separates upscaling from anti-aliasing. With upscaling off, choose
Game MSAA, SMAA 1x, NVIDIA DLAA, or FSR Native AA. Upscaling supplies its own
anti-aliasing and disables the separate selector. Separate scene AA currently
requires Vulkan and standard 16:9 output.

Game MSAA preserves guest sample counts. Its scene scale is independent of the
native output/HUD: at 1440p output, 1x means a 1280x720 scene and 2x a 2560x1440
scene. SMAA, DLAA and FSR Native AA render at output resolution and disable the
unused scene-scale control. Selecting DLAA preserves the stored MSAA scale.
Irrelevant launcher rows dim immediately, including their labels and values.

SMAA uses the official high-preset three-pass spatial algorithm before HUD
composition. Its sources and lookup tables are pinned and hash-checked; packages
include the MIT license. It needs no NVIDIA runtime. DLAA uses the existing
automatic NVIDIA runtime setup; FSR Native AA uses the existing FSR 3 bridge.
Old combined DLAA and FSR Native settings migrate to the separate selectors.

Validation: launcher runtime tests cover native scene/output dimensions,
exclusive upscaling/AA choices, legacy DLAA migration, and the ultrawide bypass.
UI tests cover NVIDIA availability and immediate disabled/re-enabled styling.
The combined build was also exercised in copied-save 1440p gameplay; that
in-game settings UI is supplied in the dependent graphics-menu change.
