# Launcher, installers and landing page

Issue #1. State: Linux path built and tested end to end; Windows path built and run once in a VM (see "Windows"), **on real
hardware**; `release.yml` ran green by hand (run 38572896631: SDK for Linux and Windows,
both launchers, Windows installer; `publish` only runs on tags and has not run).

## The idea

No prebuilt game is ever shipped (the generated C++ is translated game code). The release
holds only the **launcher**, the project's own sources, the Forza Live server and the patched
ReXGlue SDK pieces (SDK code, BSD-3-Clause). The launcher builds the game on the player's
machine from their own copy:

1. **Setup > Game files**: the player's Games on Demand package (`4D5309C9/00007000/<name>`
   plus `<name>.data/`), a disc 1 ISO (`tools/xiso_extract.py`, docs/disc.md), a bare
   `default.xex`, or an already extracted folder. A package is extracted with
   `tools/xcontent_extract.py`; `default.xex` is extracted first and checked before the long copy
   (`installer.inspect_xex`, below): title `4D5309C9`, entry point `0x82A6FBC0`, version equal to its base
   version (not patched). The supported printing (0.0.0.7, media id `2B7A1346`) is accepted on its header;
   any other printing is accepted only when its code is the same (image fingerprint). A title update
   already applied, a patch file, another game or other code is refused with the reason.
2. **Title Update 4** (recommended: Horizon Rally and the 1000 Club need it; still optional)
   and **DLC** (optional): pickers for the player's own TU4 package (the STFS package under
   `4D5309C9/000B0000`, or a zip holding it) and DLC folder. The DLC folder is listed with the
   packages' names and installed with `tools/install_dlc.py` (to the game's user data folder). It takes
   a folder of STFS package files, or DLC that is already extracted in Xenia's layout (a Xenia
   `content` folder, its `0000000000000000`, `4D5309C9` or `00000002` folder; package folders named
   like the package files), or both in one folder. Each package folder is copied as it is, with its
   `../Headers/00000002/<name>.header` when there is one (license mask set to all bits), else a
   header is generated (display name from a table of the 23 known package names, else "DLC <name>").
   Folders that do not look like DLC (a name that is not 8 to 42 hex digits, no files inside) are
   skipped and listed in the card.
   With a TU4 package the build runs `tools/apply_title_update.py BASE_XEX TU_PACKAGE
   <data>/build/tu/default.xex` (patched `default.xex`, both facade DLLs and `media.zip`, about
   2 s), copies `forza_manifest_tu4.toml` with its three `file_path`s and `game_root` pointed at
   `build/tu`, configures with `-DFORZA_TITLE_UPDATE=4` (`hints_tu4/`, `generated_tu4/`) and
   stages `build/tu` as `runtime/update`, which Play passes as `--update_data_root`
   (`--game_data_root` stays the disc's files). Without it the base manifest is built and
   `runtime/update` removed. `--build` takes `--title-update PATH` (default: the launcher's
   setting) and `--base`.
3. **Build**: preflight (RAM, disk, cores), toolchain, SDK, codegen from `default.xex`,
   CMake configure, ninja (progress parsed from `[n/m]`, log view, cancel, resume), then the
   result is staged into `<data>/runtime`. The build is repeated only when the launcher or
   project version changes, or the title update is added, removed or replaced (a hash of
   `forza-recomp/` sources, the launcher version, the SDK pin and the TU4 package). "Free disk space" deletes the build tree (about 1 GB); the game keeps working.
4. **Play**, **Settings**, log folder, versions.

Data lives in the game's user folder (`~/.local/share/forza`; on Windows `%LOCALAPPDATA%\ForzaReImagined`, or
`Documents\forza` for an install that already lives there; changeable under Setup > "Where the game is built", see "Round 5"): `game/` (extracted files), `toolchain/`, `downloads/`,
`sdk/`, `build/`, `runtime/` (what Play runs, with `forza.toml`), saves, `forza.log`.
The launcher's own config is `~/.config/forza-reimagined/launcher.json`
(`%APPDATA%\ForzaReImagined\launcher.json`).

## Toolchain (pinned in `launcher/forza_launcher/pins.py`, every archive checked by sha256)

* Linux: the system `clang` (>= 21; the project is tested with 23), `cmake` (>= 3.25) and
  `ninja` are used when present, else official LLVM 23.1.3, CMake 4.4.4 and ninja 1.13.2 are
  downloaded into `toolchain/` (LLVM is a 2 GB download; only the compiler, linker and archiver
  tools are unpacked, 0.8 GB, and the archive is deleted afterwards). The C++ standard library headers
  must come from the system (`g++`/`libstdc++` dev package: Debian `build-essential`, Fedora
  `gcc-c++`); the launcher tests a C++23 compile and says so if that fails.
* The SDK: the pinned ReXGlue nightly (`nightly-20261002-bd833a2a`, downloaded from the SDK's
  release) plus the project's patched plugin, runtime and codegen tool from
  `sdk-overlay/rexglue-overlay-<os>-amd64.tar.gz`, which CI builds and the installer carries.
  Redistribution: the SDK is BSD-3-Clause (copyright Tom Clay, portions Xenia); the binary
  form must reproduce the notice, which `licenses/rexglue-sdk.txt` in every package does.
  The overlay holds SDK code only, no game code.
* Windows (built once in the VM, see "Windows"): portable LLVM (clang-cl, lld-link), CMake, ninja downloaded the same
  way. The MSVC CRT and Windows SDK come from an installed Visual Studio 2022 (17.3+) / Build Tools
  (`vswhere`, `vcvars64.bat`; older ones are skipped, see "Round 5") if there is one, else **xwin** on the player's machine after the
  player ticks "I accept Microsoft's license" (the launcher links the license; nothing of
  Microsoft's is redistributed). The toolchain file is `tools/cmake/windows-native-clang-cl.cmake`.
  The Windows overlay is the whole SDK prefix (the SDK's prebuilt Windows plugin has no
  Vulkan backend), built by CI.

## Settings

`launcher/forza_launcher/schema.py` lists every key (about 70) with its group, type, default,
range and help text: Display (frame rate, vsync, fullscreen, window, resolution preset, render
scale, filtering, FSR/CAS, FXAA, ...), Controls (keyboard and mouse `mnk_*` with the steering ramp, 25 keybinds,
deadzones, vibration, `FORZA_PAD_PER_USER`), Audio, Online (server mode, live room, Horizon
World switches), Profile (own save or a copy of a Xenia Canary save, gamertag, language),
Advanced (log, GPU, game/data folders, extra command-line flags, environment variables,
a free-form "other settings" box for any further `forza.toml` key, a live preview of the
generated `forza.toml` and command line). Keys that hold their default are not written.
Online: public (`dyotserver.ddns.net:21200`, written as `live_server` and `FORZA_SERVER`),
local (the launcher starts the Forza Live server inside its own executable, or reuses one
on port 21201, like `play.sh`), your own, or offline. The Xenia save logic is `play.sh --xenia`
ported (copy once, `--user_data_root/--cache_root/--profile_xuid`, DLC folder shared).
`forza-launcher --dry-run` prints what Play would run; it matches `tools/play.sh`
(`--game_data_root`, `--log_file`, `FORZA_SERVER`, library path).
Defaults differ from the game's in two places: the first run sets `frame_rate = 60` and `present_effect = "fsr"` (AMD FSR 1; a launcher that already holds a choice keeps it; the old `fsr2` / `fsr3` entries become `fsr`).

### GPU choice (#52)

Many glitches depend on the graphics card, so Settings > Display starts with **Graphics card**: NVIDIA, AMD, Intel,
Steam Deck, or "Other / not sure". The launcher detects the card once (first start; the answer is kept in
`launcher.json` under `gpu`) and preselects it; the player can change it at any time.

* **Detection** (`launcher/forza_launcher/gpu.py`, no Qt): Linux reads `/sys/class/drm/card*/device/vendor`
  (0x10de NVIDIA, 0x1002 AMD, 0x8086 Intel); the Steam Deck is the DMI board name `Jupiter` (LCD) or `Galileo`
  (OLED) from Valve and wins over its AMD APU. Windows asks PowerShell (`Get-CimInstance Win32_VideoController`
  and `Win32_BaseBoard`), falling back to `wmic`; the vendor comes from the adapter name or the `VEN_xxxx` of its
  PNP id; Microsoft Basic, remote and virtual adapters are ignored. With several cards (laptop iGPU + dGPU) the
  discrete one wins (NVIDIA, then AMD, then Intel). `FORZA_GPU=nvidia|amd|intel|deck` overrides.
* **Defaults**: a new launcher gets the vendor's preset as its first-run values (backend, render scale, upscaling
  filter, frame rate, shader switches). A launcher that already holds settings keeps every one: it only learns the
  vendor. Changing the vendor asks "Use the recommended settings for X now?" (No changes only the markings);
  the button "Use the recommended settings for this card" applies them again. Reset settings restores the preset.
* **Known problems** are never forbidden: the option keeps working, with an orange line under it giving the reason
  and the issue number while it holds the problem value, and the choice is marked ("Direct3D 12 (flickers on
  NVIDIA, #46)"). The explicit choice of the player always wins.

| Card | Preset | Marked as a known problem |
|---|---|---|
| NVIDIA | Vulkan, render scale 1, AMD FSR 1, 60 fps, shader cache and background shader compilation on (scale 2 is the most you should need) | Direct3D 12 (Windows): flicker, green reflections on glass and paint (#46). Render scale 3 and above: flicker and coloured artifacts across the map (#47). Background compilation or the shader cache off: Vulkan stutter (#49, open: warm-cache stutter unmeasured). Traffic car glow in the distance (#48): no setting avoids it |
| AMD (Linux and Windows) | Vulkan, render scale 1, AMD FSR 1, 60 fps | none; render scale 3 and 4 are clean on Radeon (Vulkan/RADV, and Direct3D 12 under vkd3d-proton at 3) |
| Intel | Vulkan, render scale 1, AMD FSR 1, 30 fps | nothing reported yet; Direct3D 12 (a different render target path on Intel) and render scale 2 and up are marked as untested / slow |
| Steam Deck | Vulkan, render scale 1, AMD FSR 1, 30 fps | render scale above 1 and a frame rate above 30: the Deck ran 17 to 25 fps (#45). A 40 fps cap is not offered: `frame_rate` is 30 or 60 and above |

Tests: `python3 -m unittest launcher/test_gpu.py` (detection from a fake sysfs and PowerShell / wmic output, presets
valid for the schema, warnings, the first-run / existing-settings / player-choice flows).

## Release and CI (`.github/workflows/`)

* `release.yml` (tags `v*`, manual): `sdk-linux` (`tools/build_rexglue_sdk.sh` +
  `tools/package_sdk_overlay.sh`, cached on the patches), `sdk-windows` (clang-cl on a Windows
  runner), then `launcher-linux` (PyInstaller, `launcher/packaging/make_package.py` makes
  `ForzaHorizon1-ReImagined-linux-x86_64.tar.gz` with `install.sh`, `.desktop`, icon) and
  `launcher-windows` (PyInstaller, Inno Setup `launcher/packaging/installer.iss`: per-user
  install with Start menu and optional desktop shortcut, bundled VC++ 2015-2022 x64
  redistributable installed when missing), `flatpak` (flatpak-builder in the Flathub image, see
  "Flatpak" below) and `publish` (tags only) creating the release and uploading all three. The version comes from the tag. Nothing here needs the game or generated code.
* `pages.yml` (push to main touching `site/`): copies `docs/images` next to `site/` and
  deploys it to GitHub Pages. Enable Pages (source: GitHub Actions) in the repository settings.
* Preview the page: `tools/preview_site.sh`.

## Developing

```sh
python3 -m venv launcher/.venv && launcher/.venv/bin/pip install -r launcher/requirements.txt -r launcher/requirements-build.txt
launcher/.venv/bin/python launcher/main.py            # the window (in a checkout it also finds this tree's own build and overlay)
launcher/.venv/bin/python launcher/main.py --build --root /path --jobs 4   # the same build without the window
cd launcher && .venv/bin/pyinstaller --noconfirm forza-launcher.spec    # the frozen app
```

## Measured (8 cores, 31 GB RAM, system clang 23.1.2; another game was running meanwhile)

Clean builds through the launcher code (fresh data root, the pinned SDK nightly downloaded,
codegen, configure, 346 ninja steps, `-j4`, low priority), wall time and peak memory of the
build's process group (sampled every 2 s):

| Build | Wall time | Peak memory |
| --- | --- | --- |
| source tree, system clang 23 | 4.6 min | 1.8 GB |
| the frozen app with the bundled overlay, system clang | 4.6 min | 1.8 GB |
| frozen toolchain download: LLVM 23.1.3 (2 GB), cmake, ninja, then the same build | 4.2 min after the download (about 6 min in all) | 1.8 GB |

On disk afterwards: build tree about 1 GB, SDK 1.6 GB, runtime 0.2 GB, the unpacked toolchain
1.1 GB (only the compiler driver, linker, archiver tools and `lib/clang` of LLVM's 12 GB), the
extracted game 7.6 GB. The binary built by the launcher booted to the title screen
("PRESS START") and held 59.6 fps with the `frame_rate = 60` the launcher wrote.

## Verified / not verified

Verified on Linux:

* Extraction of a real Games on Demand package (7.6 GB) with progress, `default.xex` checked
  first (base version accepted; `/etc/hostname`, a missing path and a non-package are refused
  with readable messages); 23 DLC packages listed by name and installed.
* The full clean build above, three ways; Play from the launcher (local Forza Live server
  started and stopped with the game, `/dev/shm` clean, no stray processes); cancel at step 60
  and resume (ninja continued with 287 of 346 steps left); a download interrupted at 20 MB
  resumed with Range and passed its sha256.
* `--dry-run` output equals `tools/play.sh` for the same settings (`--game_data_root`,
  `--log_file`, `FORZA_SERVER`, `LD_LIBRARY_PATH` order; `--xenia` gives `--user_data_root`,
  `--cache_root`, `--profile_xuid`; the Xenia save copy and DLC link were run).
* DLC from a Xenia content folder (#36, 2026-10-14, Linux): `tools/install_dlc.py` and the DLC card take
  a Xenia `content` folder, its `0000000000000000`, `4D5309C9` or `00000002` folder, a folder of package
  files, or both mixed. Xenia writes the same header (0x148 bytes of XCONTENT_AGGREGATE_DATA plus a
  little-endian license mask) as this tool; its masks come from the package's STFS licenses (0, 1, 2 or
  all bits in the 23 headers of a Xenia folder), so the copy sets the mask to all bits (the runtime reads
  bytes 0x148..0x14C, `ContentPackage::LoadPackageLicenseMask`). Tested with copies of the 23 installed
  packages in scratch user data roots: with `Headers/` (masks rewritten to 0, 1 and 2, plus an empty
  folder and a folder named like no package: both skipped and listed), without `Headers/` (headers
  generated, byte for byte the originals), only the `00000002` folder, the `4D5309C9` and
  `0000000000000000` folders, a mix of package files and folders, and package files (two synthetic STFS
  packages, as before); the Setup page (Qt offscreen) listed the names and the skipped folders, installed
  23 packages and showed "23 installed". Booting the TU4 build on each of the two Xenia-style
  copies (`FORZA_DLC_LOG=1`, START and A scripted into the main menu): the game probed 20 `DLCRoot`
  mounts and answered 252 `IsCarInstalled` queries, all "installed", none "not installed".
  The Horizon Rally menu entry was not looked at in this run.
* PyInstaller (the spec bundles tools, assets and the Forza Live server's stdlib imports),
  the frozen app starts, runs the local server (`--live-server`) and a whole build; `install.sh`.
* Title Update 4 (2026-10-14, source tree, system clang 23, `--jobs 4`, nice 10, SDK downloaded):
  `--build --title-update <the TU package zip>` applied the update (2 s), generated code from
  `forza_manifest_tu4.toml` pointed at `build/tu`, built 350 steps in 3.6 min (2.0 GB peak) and staged
  `runtime/update` (patched `default.xex`, both facades, `media.zip`); the runtime booted with
  `--update_data_root=runtime/update` to the main menu, which lists HORIZON RALLY. Rebuild detection:
  the same package reads "built", no package reads "stale: the title update was removed".
  A fresh build found one bug that the in-tree build hid: `src/main.cpp` included the base
  `generated/default/forza_init.h` (present in the checkout, absent in the launcher's tree); a TU4
  build now includes `generated_tu4/`.
* Workflows: `actionlint` clean, YAML loads. Landing page: Chromium at 1440x900 and 390x844
  (no console errors, no horizontal scroll, lightbox works).

Windows run time: see "Windows" below (installer, extraction, a whole build with xwin and Play without a GPU were run
in a VM on 2026-10-14). Not verified: a Visual Studio Build Tools machine, Windows 10, the `publish` job (tags
only), the TU4 step on Linux in the frozen app.

## Open

* **Title update and DLC.** Both are optional in the UI; TU4 is marked recommended. A TU4 profile
  save (converted by the game) is refused by the base build, so a player who has one needs the
  TU4 build. A suggestion for `tools/install_dlc.py` (not edited): a `progress` callback; the
  launcher counts bytes by wrapping `xcontent_extract.extract`.
* The Windows build path (on a player's machine), the installer and the `publish` job have never
  run. The CI jobs build them; the Windows SDK overlay has not been used to build the game.
* The Linux overlay is built on Ubuntu 22.04 (CI image `ubuntu-22.04`, glibc 2.35: the binaries
  need `GLIBC_2.35` and `GLIBCXX_3.4.32`, so any distro from 2022 on that has a g++ new enough
  for the game build runs it). 22.04 has neither C++23 headers nor CMake 3.25, so
  `launcher/packaging/ci_linux_sdk_setup.sh` takes g++-14 from the `ubuntu-toolchain-r/test` PPA
  and CMake, ninja and LLVM from the launcher's pinned downloads (`ci_fetch.py`); the SDK's
  extra `-dev` packages (`libx11-xcb-dev` was the missing one) are listed there. The same script
  was run in an `ubuntu:22.04` container: configure, build and `package_sdk_overlay.sh` pass
  (about 8 min on 8 cores).
* Windows CI: `.gitattributes` marks `patches/**` as `-text` and the job sets
  `core.autocrlf false` before checkout. With the runner's default autocrlf the patches get CRLF
  line endings and `git apply` fails on the very first one (`patch failed:
  spirv_translator_fetch.cpp:2030`), which was the first dry run's failure. The clang-cl build
  of the SDK prints millions of warning lines (GitHub then cannot serve the log), so the job
  keeps the output in a file and prints only failures.
  clang-cl reads the SDK's `-Wall` as `-Weverything` (millions of warnings, a 56 minute build)
  and ignores `-fno-char8_t` and `-ffp-model=strict` (the first broke `xboxkrnl_ob.cpp`, a `u8`
  literal passed as `std::string_view`), so the job rewrites those three lines of the SDK's top
  `CMakeLists.txt` to `-w`, `/Zc:char8_t-` and `/fp:strict` before configuring. The build then
  takes about 10 minutes. Hand-run artifacts (run 38572896631): Linux launcher package 79 MB
  (starts, `--version`, `--dry-run`), Windows installer 69 MB (Inno Setup 6.7, with the VC++
  redistributable), SDK overlays 7 MB (Linux) and 18 MB (Windows: runtime, Xenos plugin, codegen
  tool, headers, import libraries, licenses; no Microsoft files).
* Code signing of the Windows installer (SmartScreen will warn).

## Flatpak (Linux, issue #1): status and next steps

Files: `flatpak/` (manifest `io.github.dyotson.ForzaHorizon1ReImagined.yml`, `.desktop`, AppStream
`.metainfo.xml` (validates with `flatpak-builder-lint appstream`), `stage.py`, `rebuild.sh`,
`forza-reimagined.sh`; the CI job `flatpak` is in `.github/workflows/release.yml`). Local build: `flatpak/rebuild.sh` (needs
`flatpak install --user flathub org.flatpak.Builder org.freedesktop.Sdk//26.08
org.freedesktop.Sdk.Extension.llvm22//26.08`; big files go to `~/.cache/forza-flatpak/`).

Design: the app's runtime is `org.freedesktop.Sdk//26.08` itself (glibc 2.44, g++ 16 headers, cmake 4.4,
ninja, python 3.14 are there at run time), and `org.freedesktop.Sdk.Extension.llvm22` (clang 22.1.8,
accepted: the launcher needs >= 21) is declared as the app's extension with `autodownload`. The
launcher runs from source with the SDK's python; PySide6-Essentials 6.12 and shiboken6 are pinned
wheels in the manifest. `stage.py` copies what the launcher needs (launcher, tools, project sources,
server, the CI-built SDK overlay, licenses; no game code, data or generated code) into
`flatpak/stage/` (git-ignored). `paths.IN_FLATPAK` (FLATPAK_ID) adds the extension's `bin` to PATH and
switches Qt to its own file dialogs (the portal would hand out the one picked file, losing a Games on
Demand package's `<name>.data`). Permissions: wayland, fallback-x11, pulseaudio, network, `--device=all`,
read-only `home`, `/media`, `/run/media`, `/mnt` (other disks: `flatpak override --user --filesystem=PATH:ro`).
Saves, config, extracted game and build live in `~/.var/app/<id>/`. New CLI: `forza-launcher --extract PACKAGE`.

Tested (this host, Fedora Atomic, user installation): the app builds and installs; `--extract` of the
real GoD package (7.1 GB, about 15 s); `--build --title-update <TU4 zip> --jobs 8` inside the sandbox with
the SDK's cmake/ninja and clang 22.1 (no downloads except the 122 MB SDK nightly): 3.4 min wall, 3.1 GB
peak build memory, TU4 applied, 357 ninja steps; the game was started from the launcher's own launch plan
(Play code path) in the sandbox on Wayland/radv with the local Forza Live server child and a copy of a
Xenia save, but it stopped a second later for want of an audio backend (fixed, see below). Repo size of the app commit 98 MB (734 MB installed with the 496 MB LLVM extension).

Bundle test and game test (2026-10-14, CI run 38594452070, all five jobs green: sdk-linux,
sdk-windows, launcher-linux, launcher-windows, flatpak; `publish` is tag-only). The `flatpak` job is
in `release.yml` (flatpak-github-actions v6.8 pinned, freedesktop-26.08 image, reuses
`sdk-overlay-linux`, the SDK's LICENSE from the pinned commit) and `publish` needs it and uploads the
bundle with the installers. Bundle: `ForzaHorizon1-ReImagined-x86_64.flatpak`, 63 MB (app 259 MB
installed; the Sdk runtime is 1.8 GB installed, the llvm22 extension 0.5 GB).

* Installing the bundle on a clean user installation (`FLATPAK_USER_DIR` scratch, nothing from Flathub
  present): `flatpak install --user --bundle` adds the Flathub remote from the bundle's runtime repo and
  pulls the `org.freedesktop.Sdk//26.08` runtime and its GL/codec extensions by itself (23 s on a fast
  line). It does NOT pull `org.freedesktop.Sdk.Extension.llvm22` (autodownload only applies to installs
  from a remote). So the player runs `flatpak install flathub org.freedesktop.Sdk.Extension.llvm22//26.08`
  once. Without it the launcher still works: it falls back to the pinned 2 GB LLVM download, but now
  also shows the one command in Setup (a warning row, build_env.FLATPAK_LLVM_HINT) and in the build
  log. README and the landing page name the command.
* `--extract` of the 7.1 GB package: 5 s (page cache warm). `--build --title-update <TU4 zip> --jobs 8`:
  3.7 to 4.0 min, 3.1 to 3.2 GB peak build memory (the 3.2 GB is for `-j8`).
* FOUND AND FIXED: the first game start crashed in the guest (`KeBugCheckEx`, SIGTRAP) right after
  "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: Audio target 'pulseaudio' not available". The CI-built
  SDK runtime (overlay, so the Flatpak AND the tar.gz) had SDL3 with no audio backend: the Ubuntu 22.04
  runner had no libpulse/ALSA dev packages when SDL3 was configured (the host-built runtime links
  libpulse, libasound, libdbus, fribidi, libthai through dlopen; the CI one had none of them).
  `launcher/packaging/ci_linux_sdk_setup.sh` now installs libpulse-dev, libasound2-dev, libdbus-1-dev,
  libfribidi-dev and libthai-dev; with the rebuilt overlay the game finds the PulseAudio socket of the
  sandbox (PipeWire host: "HyperX ... 6 ch, 48000 Hz") and runs. The Windows overlay was not affected.
* Play from the launcher's own Play button (the real window on Wayland, a scratch copy of the Xenia
  save through Settings > Profile, the local Forza Live server, nothing from `~/.local/share/forza`):
  title screen, START, main menu with SINGLE PLAYER / MULTIPLAYER / HORIZON RALLY, A, free roam in the
  save's Aventador, driven with scripted RT and left stick (FORZA_AUTOPRESS_FIFO) for 3.5 minutes at
  59.7 to 60 fps. Peak RSS of the game (VmHWM): 0.68 GB at the main menu, 1.0 GB entering free roam,
  1.13 GB after 3.5 minutes of driving. README and the landing page say "about 4 GB free" now with these
  numbers (game 1.1 GB; build 2 GB at -j4, 3.2 GB at -j8). Note: the launcher's preflight
  (`build_env.NEED_RAM_GB = 8`) still errors under 8 GB installed; it is a conservative number, not measured.
* The launcher window (not headless) runs under Qt's Wayland platform inside the sandbox (KDE Plasma 6,
  radv), with the Qt file dialogs; screenshots taken with spectacle on the host.
* Test recipe: `FLATPAK_USER_DIR=~/.cache/forza-flatpak/userdir` for a clean installation. The window
  can be driven from a script run with the sandbox's python (`flatpak run --command=sh APP -c
  'PYTHONPATH=/app/lib/python3*/site-packages python3 SCRIPT'`; the script and its output must live
  under the home folder, /tmp is private, and home is read-only except `~/.var/app/<id>`).
  The game inside the sandbox is invisible to the host's `ps` (own PID namespace): read
  `/proc/<pid>/status` from a process inside it, stop it with `flatpak kill APP`.

NOT done: the PySide6 size trim (the app is 259 MB installed, 63 MB bundled) and publishing to Flathub.
Flathub lint: the app id fails `appid-url-not-reachable` (the repo is `ForzaHorizon1-ReImagined`) and
`home:ro` needs a justification.

## Windows: real-VM test (2026-10-14, Windows 11 Pro 10.0.26200, 4 cores, 8 GB, no GPU)

VM: `dockurr/windows` in podman (KVM). To reach sshd from the host the container needs
`-e NETWORK=user -e USER_PORTS=22` (the default tap mode did not forward port 22: "connection reset");
the disk survives re-creating the container. Host memory matters: the container was OOM-killed once
when another process grew to 14 GB.

Results, release v0.1.1 `ForzaHorizon1-ReImagined-Setup-0.1.1.exe`:

* Silent install (`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /LOG=...`): exit 0 in about
  10 s; `%LOCALAPPDATA%\Programs\ForzaHorizon1-ReImagined\forza-launcher.exe`, `project\`, `sdk-overlay\`
  present; Start menu and desktop shortcuts `Forza Horizon 1 - ReImagined.lnk`; uninstall registry entry; the
  VC++ redistributable step ran (v14.44 afterwards). The desktop-icon task is checked by default, so silent
  installs get it too. Silent uninstall (`unins000.exe /VERYSILENT`) removed files, shortcuts and the entry.
* Interactive install through the wizard (license, folder, tasks, install, finish "Start the launcher"):
  all pages fine, no abort (the colon fix of 0.1.1 holds on real Windows).
* Launcher: starts from the installer's Finish page; the window renders (Play, Setup); `--version` prints
  0.1.1; `--dry-run` without game files prints the readable "not set up" message and exits 1. In a window
  of the default 1040 px width the Setup page's content is clipped on the right on Windows (Segoe UI is
  wider than the Linux font, something in that page forces a minimum width); maximized it is fine. Not
  fixed yet.
* `launcher/packaging/test_installer.ps1 -Setup <exe> [-ExpectVersion X]` automates the above (install,
  assert files/shortcuts/registry, `--version`, `--dry-run`, uninstall, assert removed); it passed on the VM.

### Round 2 (same day, VM as above; issues #26, #28, #29, #30): what was found and fixed

VM access that worked: `ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 tester@127.0.0.1`
(the host key changes when the container is re-created; without those options a script hangs on the
prompt). The Samba share (`\\host.lan\Data`) is slow to show new files to the VM (minutes of stale
directory listings): use it to move big files, and `scp` for scripts. For GUI tests a PowerShell loop
started by a scheduled task in the console session (`schtasks /create ... /ru tester /it`) ran scripts that
click (`SetCursorPos` + `mouse_event`, `SendKeys`) and take screenshots (`Graphics.CopyFromScreen`), which
`scp` brings back; the screen is 1280x800. Python 3.12 + PySide6 in the VM runs the launcher from a
`git archive` of the tree, so a fix is tested in a minute.

* **#29 (Extract game files stays at 0% and vanishes).** Extraction itself works on Windows: a real Games on
  Demand package (7.1 GB, 4 cores, a few minutes), the same package converted by iso2god 1.8.1 from the EU/Asia
  ISO, and the ISO itself (`tools/xiso_extract.py`) all extract in the GUI (source tree and the v0.1.1 frozen
  app) and with `--extract`. Root cause: the worker thread DID report its error, and the Setup page erased
  it. `SetupPage._finished` printed the message and then called `refresh()`, which, with no `default.xex`
  yet (the extraction deletes its `.part` folder on failure), blanked the message label and set the badge back to
  "Needed". Any refusal after the progress bar started (a `default.xex` that is not the base version, not enough free
  disk space, a disk error) looked like the bar disappearing. Reproduced by making `KNOWN_BASE` empty in the VM.
  Fix (e59faa1): failures are kept in `SetupPage.fail` until the next run, shown red, with a dialog and the
  path of the new `launcher.log` (`%APPDATA%\ForzaReImagined\launcher.log`, tracebacks included); `Task.run`
  catches `BaseException` (a `SystemExit` from a tool would have ended the thread without a signal), logs it and
  `sys.excepthook` shows a dialog for anything that reaches the top. The same wipe hit the build (`status3`) and
  the Play page (below). What the players' copies actually fail on is still unknown; the message now says
  (version, entry point, media id of their `default.xex`).
* **#30 (extracted folder, picked its `default.xex`, step 1 stays "Needed", no error).** A complete extracted
  folder (`default.xex` + both facade files) is accepted with "Use these files" or "Choose folder" (checked in
  the VM). A folder missing a file is refused with the file named; the same wipe made the message vanish when the
  player went on to fill the title update field (refresh). The message now says to choose the whole extracted game
  folder (the three files).
* **#29 / #30 again (v0.1.2: players still stuck at Setup; round 3).** Their `launcher.log` files showed the
  refusal the previous round made visible: `version 0.0.0.9 / 0.0.0.10 / 0.0.0.12, base the same, media id
  6368CD69 / 2DC7007B / 4000D145, entry point 0x82a6fbc0`, "it looks like a title update" (wrong: these are
  retail printings of the base game, not updates). The launcher accepted only `(version 7, entry point)`. The
  executables of these printings are the same build (docs/disc.md), so the rule is now by code, not by number:
  `forza_launcher/xexcheck.py` decodes the image (AES via `tools/apply_title_update.py`: openssl or
  `cryptography` when present, pure Python otherwise, 14 s here on the 23 MB image; runs in the extraction
  thread, in a task when "Use these files" is pressed, and the verdict is remembered per file in
  `xex_checks.json` next to `launcher.json`, so the window's frequent refreshes are free) and compares SHA-1
  prefixes of 64 KB pieces of every section (`[va, va+size)`, so padding between sections is ignored) with
  `forza_launcher/xex_reference.py` (hashes only; written by `tools/xex_reference.py extracted/game` from the
  supported files, 458 hashes for the three executables). Same for the two facade DLLs. What the verdict
  says: accepted with "This is a different printing of the game (version X, media id Y): its code is the same as
  the supported one"; refused as "A title update is already applied" (version != base: TU4 output is 0.0.4.12 on
  base 0.0.0.12), "a title update patch" (module flags), "damaged" (the image fails its own header's page
  digests) or "is not the supported code: it differs in: section .text (code) at 0x823f0000-0x823fffff ..."
  (any section, code or data, with the address ranges of the 64 KB pieces). `run_build` runs the same check,
  so a headless build cannot start from other code. Second bug in the same log: the player chose
  `<name>.data\Data0000`; `resolve_package` now maps a `DataNNNN` file (or the `.data` folder) to the package file
  beside the folder. A relative `--title-update` path to `--build` is made absolute (the title update tool ran in
  another directory).
  Tests: `python3 -m unittest launcher/test_xex.py` (header-rewritten copies of the supported files as
  0.0.0.9/10/11/12 are accepted; the TU4 output is refused as a title update; one changed byte in `.text`,
  `.data` or a facade's code is refused naming the section; a changed string in the padding after `.edata` is
  accepted; a damaged file is called damaged; `DataNNNN` / `.data` / package resolve to the package). Proof of
  the build: `--build --title-update <TU4 zip>` from a scratch root whose `default.xex` and facades have the
  header of 0.0.0.12 / media `4000D145`: accepted (message above), TU4 applied (370 of 371 page groups, the
  same page as in docs/dlc.md; the facades 61/61, 54/54), codegen and 359 ninja steps (5.2 min at `-j4`, 2.0 GB
  peak), then the game booted with a scratch profile and ran: first-run video setup, intro videos, new game, in
  the open world driving. Not verified, as no such file was at hand: the real 0.0.0.9, 0.0.0.10 and 0.0.0.12 executables;
  they are accepted only if their images are as the table above says (the fingerprint decides on the
  player's machine, and a different one is refused with section and addresses, which the player can post).
  A test trap met on the way: with `media/` and the other game folders as **symbolic links** into another copy,
  the game stops at "Disc Read Error" (`NtCreateFile ... game:\media\aiopenworld.zip -> 0xc000000f`, the file
  exists): the runtime does not follow a symlinked directory under `game:`. It is not the header (the same
  executables booted with the folders hard-linked); the launcher copies real files, so players are not affected,
  but a game folder made of symlinks (to point the launcher at a game elsewhere) does not work.
* **#26 (Setup clipped at 1040 px).** Reproduced (Segoe UI). Cause: the Windows-only "I accept Microsoft's license
  terms ..." `QCheckBox`, whose text cannot wrap, forced the scroll page's minimum width past the window.
  `pages.WrappedCheck` (a check box plus a wrapping label) fixes it; verified by screenshot at the default size.
* **#28 (`\usr\bin\true codegen`).** d33e2d4 verified: a Windows build from the CI installer of main
  (0.0.0-dev.649f0bc, frozen, Visual Studio absent so xwin) with TU4 + 23 DLC + the ISO as game files, through the
  window (Build, license ticked, `-j3`, low priority): LLVM 23.1.3 / CMake 4.4.4 / ninja downloaded, xwin splat,
  TU4 applied, 357 ninja steps, `forza.exe` linked and staged: **563 s wall (9.4 min, with the downloads) and a
  1.5 GB peak** (summed working set of the process tree, now measured on Windows through Toolhelp +
  `GetProcessMemoryInfo`; `builder._windows_tree_memory`). On disk: build 0.9 GB (after link), toolchain + xwin 1.7 GB,
  game 7.6 GB. The preflight says "8 GB installed, 5 GB free (need about 8 GB)" as an error on this 8 GB VM and asks
  "Try anyway?"; the build did not run out of memory.
* **Play without a GPU.** `forza.exe` starts, logs `Failed to create a Vulkan instance: ErrorIncompatibleDriver`
  and exits with code 1 within a second. The launcher said "The game is running." forever: `Play.show_message(warn)`
  was hidden by the `refresh()` that follows. Now (9949778) the Play page keeps a red box: exit code, a sentence
  for the usual cause (no Vulkan GPU), the last three error lines of `forza.log`, the log path.
* **Installer test in CI.** The `installer-test` job (windows-2022, `test_installer.ps1`) ran on the CI-built
  installer: every check passed, but the step exited 1 (the last native command, `--dry-run`, returned 1);
  `exit 0` added. `release.yml` run 38641731120 validates it (artifacts only).

### Round 4: #35 (`manifest 'build.ninja' still dirty after 100 tries`), found and fixed

Players on v0.1.2: `[0/100] Re-running CMake...` repeating, then ninja gives up. Mechanism (reproduced on Linux with
cmake 4.4.3 / ninja 1.13.2: `touch -d '+2 hours' inc.cmake` on a file the project includes): build.ninja's
regeneration rule depends on every CMake file the configure read; if one has a modification time newer than the
clock, CMake regenerates build.ninja (its mtime is "now", still older than that file), ninja checks again, and
after 100 rounds reports "perhaps system time is not set". Which file, on Windows:
**the installer stamped the installed files with the future.** Inno Setup stores the build machine's *local* time
by default (CI runners are UTC) and applies it as the installing machine's local time (no `TimeStampsInUTC`): a
file built at 21:40 UTC is installed "at 21:40 local", 3 h ahead for UTC-3, 7 h ahead for PDT. The toolchain file
`<install>\project\tools\cmake\windows-native-clang-cl.cmake` is passed to CMake straight from the install
folder (`-DCMAKE_TOOLCHAIN_FILE`) and is a regeneration input, so for the first hours after a release (until the
player's clock passes the stamp) every build on every machine west of UTC looped. Confirmed in the VM with the
correct clock (VM is PDT; it had been running 7 h fast, which also showed that an old clock era leaves future
times behind: `ninja -d explain -n` named, in turn, `rexglue_project.cmake`, `generated/default/dll_targets.cmake`
and CMake's own `Platform/WindowsPaths.cmake`, all stamped while the clock was wrong): with the v0.1.3 code and the
installed project, `ninja explain: output build.ninja older than most recent input
...\project\tools\cmake\windows-native-clang-cl.cmake (+6.5 h)`, both on a fresh tree and on one left by a
v0.1.1 build that stopped at 24% (#28). A stale state from an older launcher is not by itself a cause, but it
keeps whatever stamps it had.

Fixes (ad12ef1, c3078f6):

* `builder.sync_toolchain_file`: the Windows toolchain file is copied into `build/toolchain/` (a new file, so the
  installer's time never reaches CMake); `clamp_future_times` sets every file or folder with a modification time
  in the future to now, under the project copy, the SDK, the downloaded toolchain, xwin and that copy, before each
  configure (a wrong clock, an archive, an installer; it logs "N files had a modification time in the future").
* `configure_stamp`: the CMake build tree (`out/build/<os>`) records the launcher version, platform, SDK pin and
  overlay, compilers (path and source), and title-update choice in `.forza-configure-stamp`; a tree with another
  stamp, or none (made by an older launcher), is deleted and configured from scratch, not resumed.
* If ninja still prints "still dirty after 100 tries", `run_build` deletes the CMake build tree, clamps again and
  configures once more; if that fails too the message says to check the date, time and time zone, to press
  Resume build, and to delete the build folder if it comes back.
* `installer.iss`: `TimeStampsInUTC=yes`, so installed files keep real times.
* `forza-recomp/CMakeLists.txt`: `codegen_skipped.cmd` is written only when its content differs (a `file(WRITE)`
  every configure is a new time on a file the custom commands depend on; it is not a regeneration input and was not
  the cause).
* `launcher/test_build_tree.py` (clamp, stamp/reset), `python3 -m unittest launcher/test_build_tree.py`.

Verified in the Windows 11 VM (source tree of the launcher, `--build --base --jobs 4`, the installed project with its
installer times, correct clock unless noted; each ends with `forza.exe` linked and staged, 4.2-4.4 min, 1.7 GB peak):
(c) current code, fresh tree: no loop (control with the v0.1.3 code: loop, explained above); (b) clock set 5 h back
(downloaded toolchain, SDK, xwin all in the future): the control loops, the new code logs "14754 files had a
modification time in the future" and builds; (a) a v0.1.1 build stopped at 24% (the `\usr\bin\true` failure), then the new
code: "The earlier build was made by another version ... configuring from scratch", builds; the retry: with the
clamp disabled and a future-stamped toolchain copy, ninja loops, the launcher deletes the build tree, clamps and the
second attempt finishes; with the clamp disabled for good the message above is the result of the second failure.
Linux headless build (`--build --base --jobs 4`, scratch root, system clang 23.1.2 / cmake 4.4.3 / ninja 1.13.2): 350
steps, 5.6 min, 1.8 GB peak; a second run resumes the same tree (no "configuring from scratch"). The CI installer of
`release.yml` run 38654797202 (all jobs green, artifacts only): installed in the VM (PDT) at 23:39 UTC, the toolchain
file's modification time is 23:33:54 UTC, the build's own time (v0.1.3's installer gave +6.5 h); `TimeStampsInUTC`
works.

Still not tested: a Visual Studio Build Tools machine (the `vswhere`/`vcvars64.bat` path), Windows 10, a real GPU,
long user names or a data folder with spaces/non-ASCII characters, antivirus interfering.

The `installer-test` job in `release.yml` (it runs the script on the CI-built installer):

```yaml
  installer-test:
    needs: launcher-windows
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with: { name: launcher-windows, path: installer }
      - shell: pwsh
        run: |
          $exe = (Get-ChildItem installer\*Setup*.exe | Select-Object -First 1).FullName
          launcher\packaging\test_installer.ps1 -Setup $exe
```

### Round 5: #37 (Visual Studio 2017 picked) and #38 (access denied under Documents, Discussion #31)

**#37.** A player's log said `MSVC: Visual Studio at ...\2017\Professional`; every file then failed (`struct char8_t;` in
`<experimental/filesystem>`, `'bit' file not found`, `'span' file not found`): clang-cl compiles against the MSVC STL of
that install, 14.16, which has none of the C++23 the SDK headers and the project use. `vswhere -latest` had returned it
because no version was required. What the code needs (grep of the SDK's `include/` and `src/`): `<expected>` with
`std::expected`/`std::unexpected` (SDK headers, 5 + 3 uses), `std::byteswap`, `std::unreachable`, `std::popcount`,
`std::rotl`, `<span>`, `<bit>`, `std::jthread`/`stop_token`, `std::endian`; the project itself uses no further library
feature. The newest of these is `<expected>`: MSVC STL of Visual Studio 2022 **17.3 = toolset 14.33**. Rule
(`build_env.find_msvc`, constants `MIN_VS = (17, 3)`, `MIN_TOOLSET = (14, 33)`):

* `vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8` (without `-latest`)
  lists every installation, newest first; one older than 17.3 is skipped by its version without running anything;
* for the others `vcvars64.bat` is run and its environment checked by what is really installed, not by a number:
  `VCToolsVersion` >= 14.33, `<expected>`, `<span>`, `<bit>` and `windows.h` found on `INCLUDE` (a Visual Studio 2022
  can hold only the 2019 toolset 14.29, or no Windows SDK);
* the first that passes is used; otherwise the downloaded runtime (xwin, the license tick) is used as before.

The log says it, for example `MSVC: found ... 2022 ... ; ... 2017 ...`, `MSVC: skipping Visual Studio Professional 2017
15.9.36 at ...: too old: its C++ standard library (MSVC 14.1x) lacks <expected> ...`, `MSVC: using Visual Studio Build Tools
2022 17.14.5 at ... (toolset 14.44.35207)` or `MSVC: no installed Visual Studio is new enough; the Microsoft C++ runtime
and Windows SDK are downloaded instead`. The Setup page asks for the license unless this same selection finds a usable
install. Untested: a real Visual Studio 2022 (the VM has none; an old STL under clang 23 may have its own problems below
17.8, the version floor is the library's, not a tested one).
Tests: `python3 -m unittest launcher/test_msvc.py` (fake vswhere JSON: 2017/2019 skipped, newest compatible chosen, a
2022 with only toolset 14.29 skipped, missing header/SDK reasons, nothing installed). In the VM (a fake `vswhere.exe` from
C# printing JSON, fake `vcvars64.bat` files, real `cmd` run): 2017 alone, 2017 + 2022 with toolset 14.44, 2017 + 2022 with
14.29 give the logs above and the right choice; the real `vswhere.exe` 3.1.7 accepts the flags (`[]` without Visual Studio).

**#38.** `ninja: error: rebuilding 'build.ninja': GetFileAttributesEx(C:/Users/<name>/Documents/forza/sdk/.../SDL3Config.cmake):
GetLastError() = 5`: Windows denied access to the data folder, `Documents\forza` (v0.1.x default). Documents is synced by
OneDrive (Known Folder Move) on many machines and is a Controlled folder access / antivirus-guarded folder; a file ninja
cannot even stat keeps `build.ninja` dirty. Changes (`forza_launcher/datafolder.py`, `paths.py`, `builder.py`, Setup page):

* **Default.** Windows: `%LOCALAPPDATA%\ForzaReImagined` (not synced, not a protected folder). An install that already
  lives in `Documents\forza` (it has content and the new folder does not exist) keeps using it, so nothing changes under
  a working player. Linux and Flatpak are untouched (`~/.local/share/forza`, the Flatpak data dir).
* **Choose the folder (Discussion #31, issue #40).** Setup has a card "Where the game is built" (path, Change folder, Open);
  Settings > Advanced "Game data folder" is the same button and flow (it was a free text field that changed the folder
  with no check). A chosen folder that is not empty and is not one of ours gets a `ForzaReImagined`
  subfolder. It is checked: writable (a file is created, read and deleted), free space (about 15 GB, 18 with a toolchain
  download, less when the game is already extracted), OneDrive. Errors block, warnings ask.
* **Move, never delete.** Changing the folder offers to copy saves, profiles, DLC, shader cache, log and the built game
  (`runtime/`, `build/state.json`) and, separately, the extracted game files (7 GB; or leave them in the old folder, then
  `game_root` points there). The toolchain, SDK, downloads, xwin and build tree are not copied: they are made again (a
  blocked folder is exactly where they fail). The old folder is left as it is and the message says so. Copy is resumable
  (same-size files skipped) and cancellable.
* **Offer at start (Windows).** Once, when the folder in use is the v0.1.x `Documents\forza` and it is risky (inside OneDrive,
  not writable, or the last build was refused): a dialog proposes the new default; "Keep this folder" is remembered
  (`advanced.data_root_asked`).
* **OneDrive detection** (`datafolder.in_onedrive`): the folder is under a path in `OneDrive*` environment variables
  (OneDrive, OneDriveConsumer, OneDriveCommercial), a path component is named `OneDrive...`, or a folder on the way has the
  cloud attributes (recall on data access/open, offline, pinned, unpinned). Shown under the folder, logged at the start of a
  build ("Warning: ...").
* **Access denied.** Before a Windows build the folder is probed; during it, a failing command whose output has
  `GetLastError() = 5`, `Access is denied` or `Permission denied`, or a Python `PermissionError`, ends in `FolderBlocked` with
  a message naming OneDrive, antivirus and Controlled folder access, the folder, Windows' own words, and "choose another folder";
  the window offers to open the folder chooser, and remembers the folder as blocked (`advanced.blocked_root`, shown red on
  the card and used by the start offer).
* Bug met on the way: `accept_ms_license` and `build_jobs` were not keys of the config defaults, so `_merge` dropped them
  when the file was loaded (the license tick and job count did not survive a restart); they are keys now.
* Not changed (not the launcher's files): `tools/dist/play.bat`, README and the bug-report template still say
  `Documents\forza` for the zip's `play.bat`.

Tests: `python3 -m unittest launcher/test_datafolder.py` (OneDrive by environment, name and attribute; the access-denied
patterns; the message; probe; free space; defaults for a fresh and an existing install; folder choice; copy keeps the old
folder, skips rebuildable parts, resumes, cancels).

Verified in the Windows 11 VM (clock set right first: it was 7 h fast again; source tree of the launcher with the CI
installer's overlay, `--build --base --jobs 4 --accept-microsoft-license`, game files from the ISO, no `--root`):

* **Default folder.** `--extract` of the ISO went to `%LOCALAPPDATA%\ForzaReImagined\game` (7.0 GB, a few minutes); the build
  that followed ran there: LLVM/CMake/ninja downloaded, with the fake Visual Studio 2017 and a fake 2022 holding toolset 14.29
  both skipped (log lines above) and xwin used, 349 ninja steps, **7.1 min, 1.7 GB peak**, `forza.exe` linked and staged
  (so the xwin fallback still builds). The attribute check had a bug found here: `GetFileAttributesW` returns
  INVALID_FILE_ATTRIBUTES as -1 through ctypes' default `int`, which made every folder that did not exist yet look
  cloud-synced; fixed (unsigned result type). Then real folders: `%OneDrive%\forza` is flagged by the environment, a folder
  with `attrib +P` by its attribute, `Documents\forza` and the default are not.
* **Chosen folder.** The copy that "Change folder" runs (saves, runtime, state, and the 7.1 GB of game files, 6 s on the VM's
  disk), the config switched to `C:\fh2`, the old folder untouched, then a headless build with no `--root` built in `C:\fh2`
  (6.4 min, `runtime: C:\fh2\runtime`). The window flow (both questions, the check, the switch) was run against a Linux
  offscreen window with the dialogs answered by script; not clicked by hand on Windows.
* **Access denied.** Denying read on `SDL3Config.cmake` alone (`icacls /deny tester:(RA)`) did not make ninja fail (Windows
  grants attribute reads through the folder), so the player's exact ninja line was not reproduced by the ACL; the real
  `builder.run` was fed that line from `cmd` and ends in `FolderBlocked` with the message above. Denying read on the SDK's
  `cmake` folder (`icacls /deny tester:(OI)(CI)(R)`) made CMake say only "ReXGlue SDK not found": the build log shows
  that, and now `datafolder.read_problem` (listing sdk, toolchain, xwin and build, opening a few hundred files) finds the
  denied path and the build ends with the blocked-folder message naming `...\cmake\SDL3Config.cmake: Permission denied`.
  A data folder with write access denied stops before any work ("Cannot write to ..."). Trap: that deny ACE also removes
  READ_CONTROL, so even the owner's `icacls /remove` fails ("Access is denied"); `takeown /r` and `icacls /grant` repair it.
  Not tested: real OneDrive, Controlled folder access, antivirus.

`release.yml` (workflow_dispatch, artifacts only) run 38666192753 on the final commit: all jobs green (sdk-linux, sdk-windows,
launcher-linux, flatpak, launcher-windows, installer-test; publish skipped). No tag pushed, no release made. Left: a real
Visual Studio 2022 and a machine with OneDrive / Controlled folder access to confirm both fixes outside the VM.
