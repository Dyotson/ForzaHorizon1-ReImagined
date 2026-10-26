# Vendored dependencies

Only the source files and headers the build needs are included. CMake uses these
local copies without downloading dependencies.

| Directory | Used by | Upstream revision | Source archive SHA-256 |
| --- | --- | --- | --- |
| `discord-rpc` | Discord Rich Presence | [discord/discord-rpc@963aa9f3e5ce81a4682c6ca3d136cddda614db33](https://github.com/discord/discord-rpc/tree/963aa9f3e5ce81a4682c6ca3d136cddda614db33) | `8f4115999391f95f443615fc5b13cb2398926ac4c418d3d34ff5735f71c6faea` |
| `rapidjson` | Discord Rich Presence | [Tencent/rapidjson@24b5e7a8b27f42fa16b96fc70aade9106cf7102f](https://github.com/Tencent/rapidjson/tree/24b5e7a8b27f42fa16b96fc70aade9106cf7102f) | `2d2601a82d2d3b7e143a3c8d43ef616671391034bc46891a9816b79cf2d3e7a8` |
| `bc7enc_rdo` | `forza-texcompress` (texture packs, #413) | [richgel999/bc7enc_rdo@b9438627eef73a1157e84201b6fa6eb2ffd6d9f0](https://github.com/richgel999/bc7enc_rdo/tree/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0) | `bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6` |

The hashes refer to the upstream `codeload.github.com/<owner>/<repo>/tar.gz/<revision>`
archives. Verify them before updating the vendored files. Keep upstream license
notices: `discord-rpc/LICENSE`, `rapidjson/license.txt` and `bc7enc_rdo/LICENSE`. The
license files are copied beside the built game (`licenses/`) and included in launcher
runtime staging.

`bc7enc_rdo` holds only `bc7enc.{h,cpp}` (BC7), `rgbcx.{h,cpp}` with
`rgbcx_table4_small.h` (BC1) and `bc7decomp.{h,cpp}` (the BC7 decoder the tool's checks
and tests use), unchanged: MIT or public domain (Unlicense), whichever the user prefers. Its
`LICENSE` also names `bc7e.ispc` (Apache-2.0) and LodePNG (zlib), which are not
included.
