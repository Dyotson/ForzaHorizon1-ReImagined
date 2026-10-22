# Shader prebuild (#226)

Goal: like Steam's "Processing Vulkan shaders", build every pipeline the game uses before
playing, so that new places, cars and cameras no longer compile shaders mid-game (#167,
#49, #45). Nothing from the game is distributed: we ship a list of pipelines (hashes and
render state), and the shader code is read from the player's own game files.

This page is phase 1, the feasibility study. It adds three tools and changes no SDK or
launcher code:

- `tools/xxh3.py`: XXH3-64 in pure Python (the SDK's shader and pipeline hash).
- `tools/fxobj_shaders.py`: finds the shader microcode in the game files and measures how
  much of a played shader cache it covers.
- `tools/pipeline_list.py`: turns stored pipeline descriptions into a shippable list,
  merges and checks lists, and (experiment) writes SDK storage files from a list and the
  player's files.

## What the SDK stores today

`VulkanPipelineCache::InitializeShaderStorage` (`sdk/src/graphics/vulkan/pipeline_cache.cpp`)
keeps two files per title in `<cache_root>/shaders/shareable/`, and patch 0086 adds a third
in `shaders/local/`. `<cache_root>` is `<user data>/cache`. With split scene/HUD rendering
(native scene, DLSS, FSR 3, SMAA) it is
`cache/fh1-live-v1-<output W>x<H>-<scale X>x<Y>/ui` and `.../scene`, one store per
setting.

**`4D5309C9.xsh`: shader microcode.** Header: `XESH`, then version `0x20201219`
(big-endian). Then one record per shader:
- u64 hash;
- u32 with the dword count (31 bits) and the type (top bit: 0 vertex, 1 pixel);
- the microcode as it was in guest memory (big-endian words).

The key is `XXH3_64bits()` of exactly those bytes: what `IM_LOAD` pointed at. A record that
fails its hash ends the file.

**`4D5309C9.fbo.vk.xpso` (`.fsi.` for the interlock path): pipeline descriptions.** The
12-byte header holds `XEPS`, the API (0 fbo, 1 fsi), and a version (big-endian):
`max(PipelineDescription::kVersion, SpirvShaderTranslator::Modification::kVersion)`, today
`0x20261018`. If the header does not match, the whole file is dropped. Then one record per
pipeline: the u64 XXH3 of the description, then the `PipelineDescription`:

| Field | Meaning |
| --- | --- |
| VS hash, PS hash (u64) | `.xsh` keys; PS 0 means no pixel shader |
| VS / PS modification (u64 each) | translation variant: interpolator masks, dynamic register count, clip planes, host vertex shader type (vertex, domain, point or rectangle expansion), point parameters, depth/stencil output mode, contraction |
| RenderPassKey (u32) | MSAA samples, used attachments, depth format (D24S8 / D24FS8), 4 colour view formats, transfer formats |
| state (45 bits) | geometry shader type, topology, primitive restart, tessellation mode, depth clamp, polygon mode, culling, front face, rasterizer discard, depth write/compare, stencil test and ops |
| 4 render targets (26 bits each) | blend factors and ops, colour write mask |

The bit fields are packed differently per compiler. Clang/GCC (Linux, macOS) write
58-byte descriptions (66-byte records). MSVC (Windows) starts the last 13 state bits in a
new u32: 60 bytes, 68-byte records. The bit positions are otherwise the same, but the
files and the description hashes are not portable between Windows and the others. So the
shipped list uses decoded fields, never raw records.

**`shaders/local/<title>-<vendor>-<device>-<driver>-<versions>.vkpc`** (patch 0086): the
driver's `VkPipelineCache` data, opaque and at most 64 MiB.

**Warm-up.** At start the SDK reads the `.xpso` and drops records that fail their hash or
`ArePipelineRequirementsMet()`. It loads every `.xsh` shader and translates each (hash,
modification) pair the descriptions need to SPIR-V, with the current translator. It then
creates the pipelines on 3/4 of the cores, blocking, reporting `SHADER_WARMUP` progress.
No translated code is stored: only guest microcode, descriptions and the driver's own
cache. Pipelines created during play are appended to both files and flushed at the end
of every submission.

## Where the microcode is in the game files

A `.fxobj` is Forza's effect container. Each compiled shader in it is an Xbox 360 D3D
shader blob, preceded by its byte length (big-endian u32). The blob, all big-endian:

- **Header, 0x24 bytes.** Magic `0x102A1101` (vertex) or `0x102A1100` (pixel), virtual
  size, physical size, debug info offset (0x24, a `c:\Temp\UPDB\...` path, or 0), constant
  table offset, definitions offset, shader description offset, 0, 0.
- **Virtual part** (`virtual size` bytes from the start). Header, debug info, constant
  table (the D3D constant table with `vs_3_0`/`ps_3_0`), definitions, shader description.
  The description starts with u32 *physical offset* and u32 *byte size* of the microcode,
  followed for vertex shaders by the vertex fetch table.
- **Physical part** (follows the virtual part). Literal constants, then the microcode at
  `physical offset`. The microcode ends with a 12-byte compiler tag: `NJ`, u16, and an
  8-byte build id. The tag is uploaded too, so it is part of the hashed bytes.

`fxobj_shaders.py` scans for the magic and validates each header, so the effect format
around the blobs need not be parsed. The same blobs, with or without the length prefix,
are in:

| Source | Files | Blobs (VS / PS) |
| --- | --- | --- |
| `media/shaders/**/*.fxobj` | 174 | 1514 / 1407 (2918 unique) |
| title update `media.zip`, `shaders/cars/*.fxobj` (replace the base car shaders) | 6 | 1309 / 1221 |
| `media/tracks/*/bin.zip`, `shaders/track/*.fxobj` (zip method 21, LZX, no local headers) | 197 | 766 / 606 |
| executable image (engine shaders: post, sky, UI, particles) | 1 | 192 / 290 |

No other media zip holds shaders: `aiopenworld`, `animatedobjects`, `camera`,
`dynamicpost`, `effects`, `gamemodes`, `gametunablesettings`, `Livery`, `physics`,
`realtimesky`, `renderscenarios`, `Spectators` and `UI` contain XML, textures (`xds`, `dds`),
geometry and UI data. The `.sh` files in the Colorado zips are lighting data. The
launcher can already decode the executable image (`xexcheck.py` through
`tools/apply_title_update.py`, pure Python where needed). LZX members decode with
`tools/lzx_delta.py`.

### Vertex shaders are bound at draw time

Pixel shaders are uploaded exactly as stored, so their hash is the blob's. Vertex shaders
never are: when the game draws, the console's D3D runtime binds the vertex shader to the
vertex declaration and the pixel shader, and uploads the patched copy. Comparing the
stored shaders with the blobs shows three kinds of change, and nothing else:

1. **Vertex fetches** take the vertex layout. Fetch constant and slot, prefetch count,
   destination swizzle, format and its flags, exponent adjust, mini-fetch, stride and
   offset (`FETCH_LAYOUT_MASK`).
2. **Exports** are renumbered to the pixel shader's inputs. The export destination
   registers and the clamp bit change (`EXPORT_MASK`, ALU word 0 of exports only).
3. **Exports the pixel shader does not read** become a fixed no-op ALU instruction
   (`ALU_NOP`).

The list stores, for each played vertex shader, the blob's hash and those fields per
changed instruction: instruction indices and vertex layout values, no instruction code.
`apply_binding()` rebuilds the exact shader, and its XXH3 must equal the listed hash.

## Coverage

These are the shaders stored by every play session on the test machine: the main cache
from long play, and 347 stores from scripted runs. Sources: the base game files, the TU4
`media.zip` and the TU4 executable image (`fxobj_shaders.py coverage`).

| Shaders found in | VS (806) | PS (909) | All (1715) |
| --- | --- | --- | --- |
| `media/shaders` only | 37.5% | 31.6% | 34.3% |
| + TU4 `media.zip` | 73.0% | 61.3% | 66.8% |
| + track zips | 85.7% | 86.2% | 86.0% |
| + executable image | **99.4%** | **99.7%** | **99.5%** |

How the 801 vertex shaders are bound: vertex layout only 691, layout and no-ops 41,
layout, renumbered exports and no-ops 68, layout and renumbered exports 1. The 8 not found
are 5 vertex shaders that differ in other ways and 3 pixel shaders with no blob.

At the pipeline level, both shaders are rebuildable for 1568 of the 1612 distinct stored
descriptions (97.3%; all three storage versions), and for 1008 of the 1047 from the main
cache and the current version (96.3%). The rest stay as today: compiled the first time
they are met.

Without the binding step no vertex shader would match at all: a stored vertex shader's
hash never equals its blob's.

## The pipeline list

```
tools/pipeline_list.py make <cache>/shaders/shareable/4D5309C9.fbo.vk.xpso \
    --xsh <cache>/shaders/shareable/4D5309C9.xsh \
    --sources <game>/media/shaders --sources <update>/media.zip \
    --sources <game>/media/tracks --sources <image>.bin -o run.json
tools/pipeline_list.py merge a.json b.json -o list.json   # refuses mixed storage versions
tools/pipeline_list.py check list.json
```

A list is JSON with:
- `format`, `title` and `storage_versions`;
- `pipelines`: the decoded description fields, plus `partitions` (`main`, `ui`, `scene`);
- `shaders`: one recipe per hash. `file`: the blob with this hash. `bound`: `base` hash
  plus `fetch` / `export` / `nop` fields. `missing`: not in the game files.

`check` refuses anything else. Every value must be a known bit field in range, every
`fetch` and `export` value must fit its mask, and the only 64-bit strings are hashes and
modification bits. So microcode cannot hide in a list. With
`make`, the `.xsh` microcode is only read to find each shader's source and never written
to the list.

Sizes: the main cache's 940 pipelines and 951 shaders give 1.7 MB of JSON. All 347 stores
give 1612 pipelines and 1567 shaders: 3.0 MB, 84 KB gzip. A compact binary form would be
a few hundred KB.

## Building from the list

`pipeline_list.py write-storage list.json --sources ... --out <cache>/shaders/shareable`
rebuilds every listed shader from the given game files and verifies its hash. It writes a
local `.xsh` and an `.xpso` in the SDK's own format and layout, which today's warm-up
loads as is. **Experiment:** see the results below.

For the shipped feature, the SDK should read the list itself rather than the launcher
writing its private formats. The estimate:

| Change | Size |
| --- | --- |
| SDK: read a second, imported store (`shaders/prebuild/`: the launcher's `.xsh` of rebuilt shaders, plus the list in a small fixed binary of the decoded fields), filter it like the own store, merge it before translation; the existing loop translates, creates and reports progress | ~150 lines in `InitializeShaderStorage` (split the reader into a helper) |
| SDK: fix the rejected-record truncation (below); without it, imported records the device rejects would erase the player's newest pipelines at every start | ~15 lines |
| SDK: device-dependent fields. Drop or remap records the device cannot build (D24S8 depth when `VK_FORMAT_D24_UNORM_S8_UINT` is missing, `fsi` vs `fbo`, the portability-subset limits `ArePipelineRequirementsMet` checks) | ~30 lines |
| SDK: a prebuild-only run (cvar: warm up, save the driver cache, exit) | ~20 lines |
| SDK: `.vkpc` limit | raise 64 MiB, or warn when exceeded (now silent) |
| Launcher: "Prebuild shaders" step. Read the player's `.fxobj`, zips and executable image, rebuild the listed shaders (`fxobj_shaders.py`, `pipeline_list.py` logic), write `shaders/prebuild/`, run the prebuild-only game with the existing warm-up progress, offer again after a driver or launcher update | ~300 lines plus tests |

About one SDK patch of 200 to 250 lines, plus the launcher step. No change to translation
or pipeline creation.

Partitions: with split rendering the scene and HUD caches are separate stores, and
scene pipelines translate with the scene's resolution scale. The prebuild run must use
the player's current graphics settings, and a settings change that selects a new
partition needs a new prebuild (offer it, as for driver updates).

## Stale caches (#167)

Findings:

- **Stored data carries format versions, not a translator or SDK version.** `.xsh`
  carries `0x20201219` (record format) and `.xpso` the `max()` of the description and
  modification versions. `.vkpc` is keyed by device, driver, pipeline cache UUID and both
  versions.
- **A translation change cannot leave stale translated code.** Only microcode and
  descriptions are stored. Every start re-translates with the current translator and
  re-creates the pipelines, and the driver's caches are keyed by the SPIR-V. After an
  update that changes translation, the warm-up is slower once while the driver recompiles;
  the cached pipelines are still correct.
- **An update that changes descriptions without a version bump** leaves dead records:
  warmed at every start, never used. The changed pipelines are compiled once in game and
  stored, and do not come back next session. A bump can also be hidden by the `max()`:
  raising the lower of the two versions changes nothing.
- **Bug: rejected records make the newest pipelines disappear at every start.** Upstream
  `pipeline_cache.cpp` (bd833a2: lines 461-491 and 695) skips records that
  `ArePipelineRequirementsMet()` rejects and compacts the kept ones in memory. It then
  truncates the file to *kept count x record size*, but the file is not compacted. The
  rejected records stay in place, and the last N valid records are cut off, N being the
  number of rejected ones. Those are the pipelines met last session. They compile again in
  the same places, are appended, and are cut again at the next start. This happens every
  session until the cache is deleted: exactly the #167 report. It needs a store with
  records this device rejects: a GPU switch (patch 0054's discrete preference, an eGPU, a
  copied cache), a driver exposing fewer features, or any imported list. The 347 local
  stores have none, so it was not triggered here.
- **The `.vkpc` stops being saved past 64 MiB, silently** (`pipeline_cache.cpp`:486), and
  on AMD's Windows driver it grows at every start. In the RX 6700 XT log in #167, the
  start supplied 39.6 MB for 867 stored pipelines. The warm-up still took 7.2 s and the
  first save afterwards was 55.9 MB: about a third of the stored pipelines missed the
  supplied data and were compiled again. The session ended at 58.4 MB, so the next start
  or two cross 64 MiB. From then on nothing new is saved. Every start recompiles what is
  missing (slow starts), and deleting the cache resets it. At about 46 KB per pipeline on
  that driver, a full prebuild (1500+ pipelines) is over the limit by itself. Why the
  warm-up misses AMD's cache for the same pipelines is open: it reuses the data on RADV
  and NVIDIA.

Proposed fix:
1. Rewrite the `.xpso` compacted when anything was rejected, instead of truncating.
2. Store the two versions separately, so that any bump invalidates.
3. Add a translation revision that our SDK patches bump when they change SPIR-V output or
   description semantics. Put it in the `.vkpc` identity, so that stale driver data is
   dropped instead of growing toward the limit. The launcher compares it to offer
   "Prebuild shaders" again.
4. Raise the `.vkpc` limit, or split the file, and log when a save is skipped.

## Phase 2 plan

1. SDK patch: the store fixes above (1-4), and the import of `shaders/prebuild/` with
   device filtering, plus the prebuild-only run.
2. Capture lists from scripted runs that cover the map, every car class, the cameras,
   photo mode, Autoshow and the UI scenes. `tools/perf_scenario.sh` and `live_session.sh`
   on Linux AMD, Windows NVIDIA and the Steam Deck. Merge per storage version. Keep D24S8
   and D24FS8 variants, or let the import remap them.
3. Launcher: the "Prebuild shaders" step in Setup, offered again after a driver,
   launcher or graphics-setting change. Progress and cancel, using the existing warm-up
   progress lines.
4. Measure: hitches per session (`FORZA_STALL`) on a fresh cache, before and after, on
   AMD and NVIDIA. Also prebuild time and warm start time with the full list.
5. Later: lists uploaded by players through Forza Live. The format already holds no
   game content.
