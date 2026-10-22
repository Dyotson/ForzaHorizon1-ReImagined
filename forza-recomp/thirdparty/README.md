# Discord integration dependencies

Only source files and headers needed by the Discord Rich Presence client are
included. CMake uses these local copies without downloading dependencies.

| Directory | Upstream revision | Source archive SHA-256 |
| --- | --- | --- |
| `discord-rpc` | [discord/discord-rpc@963aa9f3e5ce81a4682c6ca3d136cddda614db33](https://github.com/discord/discord-rpc/tree/963aa9f3e5ce81a4682c6ca3d136cddda614db33) | `8f4115999391f95f443615fc5b13cb2398926ac4c418d3d34ff5735f71c6faea` |
| `rapidjson` | [Tencent/rapidjson@24b5e7a8b27f42fa16b96fc70aade9106cf7102f](https://github.com/Tencent/rapidjson/tree/24b5e7a8b27f42fa16b96fc70aade9106cf7102f) | `2d2601a82d2d3b7e143a3c8d43ef616671391034bc46891a9816b79cf2d3e7a8` |

The hashes refer to the upstream `codeload.github.com/<owner>/<repo>/tar.gz/<revision>`
archives. Verify them before updating the vendored files. Keep upstream license
notices: `discord-rpc/LICENSE` and `rapidjson/license.txt`. Both license files are
copied beside the built game and included in launcher runtime staging.
