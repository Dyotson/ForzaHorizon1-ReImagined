# World detail beyond the console (#361)

**Settings > Display > World detail** has two options; both default to the console's values
and apply from the next start (the game reads them when it loads the track):

| Launcher | cvar | Console | High | Ultra |
| --- | --- | --- | --- | --- |
| Shadow detail | `world_shadow_detail` | small objects cast shadows up to 60 m x their size factor | 3x as far (180) | 10x (600) |
| Distant texture detail | `world_texture_detail` | the track's texture LOD bias, 0 | -0.5 | -1 |

`src/hooks/world_detail.cpp` changes the values after the game reads the track's
`media/tracks/<track>/TrackSettings.xml` (loader `sub_82D505C0`, Title Update 4
`sub_82E128B8`; the struct layout is the same in both builds). Both values are read again
every frame, so a debugger or `FORZA_TRACK_SETTINGS="1272=600,1404=-1"` (float overrides by
struct offset, a test aid) changes them live.

## What the game has, and what each knob does

- **Shadow casters, `<screenAreaDistances depth="60">`** (struct +1272). The view setup
  (TU4 `sub_82E91710`) copies the four `screenAreaDistances` into the cull records of its
  views (cube map faces, mirror, main view, shadow/depth view), each with the camera position,
  the distance D, D² and a projection factor k. An object is drawn in that view only while
  k·d² <= D²·size (TU4 `sub_8246FE78`, called per PVS item by `sub_8246FED8`; items with flag
  0x00400000 skip it). For the shadow view D is 60: trees up a hillside, posts and bushes stop
  casting shadows a few tens of metres away. Live test at a forest road: D = 0.5 removes the
  trees' shadows on the slope, 60 leaves 32 objects a frame without a shadow, 600 22, 6 104.
- **Texture LOD bias, `<MipMapLodBiasModifier value="0">`** (+1404), added by the track pass
  (TU4 `sub_82469B18`) to the bias of the track's textures. +4 visibly blurs the road and verge;
  -0.5/-1 picks sharper mip levels for the road, gravel and terrain further out. Ultra can
  shimmer slightly on fine patterns; 8x-16x anisotropic filtering (Display > Picture) helps.

Looked at and **not** made options:

- **Model LOD and draw distance of the world.** Colorado is built from authored meshes per
  area (`TERR_..._PATCH_`, `_MIDDIST_`, `_LOD00`/`_LOD01`/`_LOD02`, `_NOLOD`), and which of them
  are drawn is decided per PVS zone in the track data: `tracks/colorado/bin.zip` holds 3,502
  `__R00Znnnnn.pvsz` zone lists (zone 394: 1,444 item entries) that the game streams as the
  camera moves, and records into command buffers per zone (`<CommandBufferRecording>`). There
  is no run-time LOD distance to scale; the pop-in seen while driving is the zone switch.
  Raising it means rewriting the zone lists to keep finer meshes in more zones, which streams
  and keeps more models in the console's 512 MB guest memory: inside the streaming system,
  with stall and out-of-memory risk. Skipped.
- **The main view's size test, `<screenAreaDistances mainScene="350">`**: the same test as the
  shadow casters for the main view. In free roam it culls about 3 objects a frame; 1,000 or
  3,000 changed neither the culled count nor the draws. No visible gain.
- **Shadow map size**: the shadow cascades are EDRAM render targets (1040x2528 and 520x1264
  per frame), so the render scale (Display > Picture) already sharpens them with the rest of
  the frame. `ShadowEnds` (`InGameShadowEnd` 202, `TrackSegCamShadowEnd` 200) are not read
  in free roam (no access in a minute of driving, camera changes, pause and map); 15 changed
  nothing on screen. The command-line option `UseSmallShadowMap` only switches a
  512 to a 256-wide viewport of a smaller shadow pass.
- **Grass**: `GrassFade*`, `GrassLOD*` and `GrassMin/MaxNumShells` are read every frame but
  Colorado's grass is meshes (`GrassMaxNumShells` 0); a 1-2 m fade or 0 and 8 shells changed
  nothing.
- **Cube map (car reflections)** `<Cubemap MeshLimit FarDistance VisibilityThreshold>`,
  `screenAreaDistances cubeMap`: the reflections of the world in the cars. Cars already look
  right; left for later. `<CollidableDropDistance>` (300): no visible change at 5 m.
- **Car and crowd LOD** (`media/renderscenarios.zip`: `CarLODTransDist`,
  `CarLODTransDistTraffic`, `CrowdLODCountScales`; `TrackSettings <Crowds lod0-3>`): known
  and cheap to raise, but lowest priority (#361).

## Cost

Free roam, TU4, RX 9060 XT (RADV), 1280x720 scene, uncapped (`--frame_rate=240`), the car
standing; the three levels switched live in the same session (4 x 12 s each, the first 2 s
of each phase skipped), BENCH lock held. Command processor = its CPU ms per frame
(`fh1_frame_cost_csv`), GPU = the process's DRM engine time per frame.

| Spot | Cores | Level | fps | Command processor ms | GPU ms | Draws |
| --- | --- | --- | --- | --- | --- | --- |
| Spawn (forest hill) | 8 | console | 157.8 | 5.07 | 5.23 | 3,289 |
| | | high | 158.0 | 5.03 | 5.22 | 3,263 |
| | | ultra | 157.1 | 5.08 | 5.26 | 3,275 |
| Spawn | 4 (`taskset 0-3`) | console | 153.0 | 5.05 | 5.37 | 3,207 |
| | | high | 153.9 | 5.10 | 5.37 | 3,308 |
| | | ultra | 155.3 | 5.05 | 5.31 | 3,262 |
| Forest road | 8 | console | 146.7 | 5.67 | 5.58 | 3,501 |
| | | high | 149.3 | 5.60 | 5.55 | 3,510 |
| | | ultra | 143.6 | 5.74 | 5.63 | 3,495 |

Within the noise of the measurement (about ±2% fps): the extra shadow casters are a few to a
few tens of draws, and the texture bias costs no measurable GPU time here. The built-in
benchmark is too noisy for this: the same settings gave 4,300 to 5,200 draws a frame and
104-121 fps from race to race (console 104.5 and 107.7, high 114.9, ultra 116.5 and 119.6).
No streaming stalls (`FORZA_STALL`) or artifacts were seen in these runs.

## Screenshots

Kept out of Git. Live A/B at the same moment (2560x1440, render scale 2): the spawn's forest
hill (Ultra adds the shadows of the conifers further up the slope; the road edge and verge
sharpen) and a stop above the valley with the mountain view. On a run where Shadow detail
went from 60 to 0.5 and to 6,000 at a forest road, the trees' shadows on the slope disappear
and return.

These knobs could also sit in the in-game Graphics screen (#116): both values are read
every frame, so a live change only needs the settings struct the hook already sees.
