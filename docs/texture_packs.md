# Texture packs (#384)

Higher-resolution textures without touching the game's files: the renderer recognises a
texture the game loads by a fingerprint of its contents, and draws a file named after that
fingerprint instead. The game itself never sees the file; it keeps its own texture, in its own
memory, and only the graphics card's copy changes. This is milestone 1 of #384 (dump and
replace); how packs are made and shared comes later.

**Settings > Display > Texture packs**:

| Launcher | cvar | What it does |
| --- | --- | --- |
| Dump textures | `texture_dump` | Saves each texture the game loads, once, to `textures/dump/` |
| Use texture packs | `texture_replace` | Draws textures from the files in `textures/` (Vulkan) |
| Open textures folder | | Opens `textures/` in the game's data folder |

Both are off by default and apply from the next start. The folder is `textures` in the game's
data folder (`~/.local/share/forza/textures` on Linux, `%LOCALAPPDATA%\ForzaReImagined\textures`
on Windows): the launcher passes it as `texture_pack_root`; without the launcher it is
`<user_data_root>/textures`. Other cvars, for `forza.toml` or the command line:

- `texture_dump_png` (on): with `texture_dump`, also write a `.png` of the largest level.
- `texture_replace_preload` (off): read every pack file at start instead of when its texture
  first loads, so no texture is shown at the original resolution first (more memory).
- `texture_pack_root`: another folder.

## Dumping

With **Dump textures** on, drive where the textures you want to replace are. Each texture is
written once (also across runs: the files already in `dump/` are skipped) as

- `textures/dump/<fingerprint>.dds`: the texture as the game has it, with all its mip levels,
  in the matching DDS format (DXT1/3/5, ATI1/ATI2 for BC4/BC5, RGBA8, R8 or RG8);
- `textures/dump/<fingerprint>.png`: its largest level, decoded, to browse;
- a line in `textures/dump/index.tsv`: fingerprint, the game's format, size, mip levels, file
  format.

The fingerprint is 16 hexadecimal digits, for example `b5fcb5a5a9d78d71`. It is a hash
(xxHash3) of the texture's largest level as the game wrote it to memory (tiled, big-endian;
only the texels' blocks, not the padding around them, which holds whatever the memory held
before) plus its format, size, pitch, tiling and endianness. Where the texture is in memory is
not part of it, so it is the same in every run and on every PC; nor are its smaller mip levels
(the game binds some textures with different mip counts).

What is dumped: 2D textures in the formats DXT1, DXT2/3, DXT4/5, DXN, DXT5A, 8, 8_8 and
8_8_8_8, from 8x8 up. Skipped, and counted in the log: render targets the GPU drew (shadows,
reflections, the screen), screen-sized textures (1280x720 and its halves: render targets the
game reads back), textures the game rewrites as it runs (reloaded three times, each within a
second: videos, gauges), cube maps, 3D textures and arrays, and the other formats (5_6_5,
4_4_4_4, CTX1 and the float and depth formats) for now.

A race from the festival (3 minutes from the start) dumps about 1,300 textures, 190 MB (90 MB
of DDS files, 100 MB of PNGs); a free-roam drive from there adds about 700. The files are
untiled and written on a worker thread. If the game loads textures faster than they are
written (4,096 or 256 MiB waiting), the rest are not dumped this time and the log says how
many: they are dumped the next time they load.

## Making a replacement

Put a file named `<fingerprint>.dds` or `<fingerprint>.png` anywhere in `textures/` (folders
inside it are fine, `dump/` is not read). Keep the name of the dumped texture you started from.

- **Size**: any, as long as the aspect ratio is the same (a 512x256 texture can become
  1024x512 or 4096x2048, within a texel). Up to 16384.
- **Mip levels**: optional. A `.png`, or an uncompressed DDS without mips, gets them made
  (box filter). A block-compressed DDS (BC1-BC7) is used with the levels it has: include
  them, or the texture shimmers in the distance.
- **Channels**: as in the dumped file. Single-channel textures (8, DXT5A) are read from red;
  two-channel ones (8_8, DXN: normal maps) from red and green. Alpha comes from the file.
- **Formats**: DDS with BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC4 (ATI1, BC4U), BC5 (ATI2, BC5U),
  BC7 (DX10 header), RGBA8 or BGRA8 (32-bit RGB masks, or DXGI R8G8B8A8/B8G8R8A8), R8
  (luminance, or DXGI R8) and RG8 (DXGI R8G8, or luminance + alpha); sRGB variants are read as
  the plain ones, as the game applies its own gamma. PNG: 8 bits per channel (16-bit images are
  reduced), any colour type. Not supported: cube maps, arrays, volume textures, other DXGI
  formats.
- **Row padding** (uncompressed DDS): rows are read packed unless the header's pitch
  (`DDSD_PITCH`) says otherwise. Rows padded to 4 bytes are read in every mip level (as
  Direct3D 9 tools write them); other padding only in a file without mips. A header whose pitch
  is shorter than a row, or a file shorter than its header says, is not used (the log says
  why).
- When both a `.dds` and a `.png` exist, the `.dds` is used; the same name in two folders, the
  first path in alphabetical order.

The replacement shows from the frame after it is read (a worker reads and decodes it the first
time its texture loads, and the frame start uploads it): the original is drawn until then,
without a stall. The log names the first replacements it uses, and every file it cannot use
and why (a wrong aspect ratio, an unsupported format).

## Limits

- Replacing needs the Vulkan renderer; Direct3D 12 dumps only.
- A texture the game samples as signed data keeps its original (the replacement is only bound
  where the game reads it unsigned, which is how it reads colour textures).
- The game's own texture still loads (its memory and its copy on the graphics card), so a
  replacement adds graphics memory; replacements stay loaded until the game exits. The log's
  summary line counts them (`MiB of GPU memory`).
- The mip level the GPU picks follows the replacement's size, so the distance where a texture
  turns blurry moves out, as it should; the game's mip clamps (`mip_min_level`) are levels of
  the new image, a little sharper than before.
- Only what the list above dumps can be replaced: no cube maps (sky, reflections), 3D textures
  or other formats yet.

## Sharing packs

The project does not host packs: they are derived from the game's art. A pack must never
contain the game's original textures, the dumped files or files only re-saved from them; and
never share a dump. Share only your own work.

## The log

At most once a minute while something happens; the counts are since the start:

```
Texture packs: command processor time 6.4 ms (1.70 ms since the last summary; hashing 5.8 ms,
dump copies 0.0 ms; at most 924 us for a load); 1902 loads fingerprinted (107.0 MiB); dumped 0
(0.0 MiB written, 0 queued, 0 dropped, 0 failed); replacements for 22 textures (22 files read,
0 not usable, 22 uploaded, 66.8 MiB of GPU memory); textures skipped: 3 format, 6
cube/3D/array/scaled, 27 screen-sized, 51 dynamic, 0 mips only, 59 tiny; 164 GPU-written loads;
formats not handled: k_32_FLOAT 1, k_24_8 1, k_2_10_10_10 1
```

The command processor time is what fingerprinting cost the thread that feeds the GPU (every
frame waits for it): it runs when a texture's data loads (created, or reloaded after the game
wrote its memory), never per draw. With both options off it does not run at all.

## Cost

Measured on a Ryzen 7 7800X3D with a Radeon RX 9060 XT (Linux), TU4, a copy of the Xenia
save, warm shader cache, alone on the machine: one run of each from boot to the festival race
at the spawn and 26 s of racing (`tools/perf_scenario.sh race` inputs; about 3 minutes, 5,400
frames). "Texture pack time" is the summary line's command processor time; "per draw" the
`GPU minute` line's command processor CPU time per draw in minutes 2 (loading, free roam, the
race's start) and 3 (racing), which evens out the runs' different draw counts:

| | Texture pack time | A frame on average | Longest load | CPU per draw, minute 2 / 3 | Race p99 frame |
| --- | --- | --- | --- | --- | --- |
| Both off | not run (one test per texture load) | 0 | | 1.71 / 1.57 us | 34.3 ms |
| Use texture packs (28 files, 22 used) | 6.4 ms (hashing 5.8 ms) | 1.2 us | 0.9 ms | 1.67 / 1.58 us | 36.3 ms |
| Dump textures (empty folder, 1,226 dumped) | 36.1 ms (hashing 5.8 ms, copies 28.3 ms) | 6.7 us | 3.1 ms | 1.73 / 1.61 us | 36.5 ms |

Hashing costs about 3 us per texture load (1,900 loads, 107 MiB in the run); dumping adds a
copy of each new texture's data, once (up to 3 ms for one load). The frame rate held 30 fps in
all three; the differences per draw are noise.

## Code

SDK patch 0280 (`docs/sdk_patches.md`): `src/graphics/pipeline/texture/pack.{h,cpp}` (cvars,
fingerprint, the dump writer with its untiling, DDS and PNG encoders, the replacement reader and
its two worker threads), the hook in `TextureCache::CommitPreparedTextureLoad`, and the Vulkan
texture cache's replacement images (`VulkanTextureCache::UploadTexturePackReplacement`,
`GetTexturePackView`). The game sets `texture_pack_root` (`src/forza_app.h`); the launcher's
options are in `launcher/forza_launcher/schema.py`.
