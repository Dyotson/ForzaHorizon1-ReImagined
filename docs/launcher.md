# Launcher, installers and landing page

Issue #1. State: Linux path built and tested end to end; Windows path written but **not
tested on Windows**; `release.yml` ran green by hand (run 38572896631: SDK for Linux and Windows,
both launchers, Windows installer; `publish` only runs on tags and has not run).

## The idea

No prebuilt game is ever shipped (the generated C++ is translated game code). The release
holds only the **launcher**, the project's own sources, the Forza Live server and the patched
ReXGlue SDK pieces (SDK code, BSD-3-Clause). The launcher builds the game on the player's
machine from their own copy:

1. **Setup > Game files**: the player's Games on Demand package (`4D5309C9/00007000/<name>`
   plus `<name>.data/`), a bare `default.xex`, or an already extracted folder. A package is
   extracted with `tools/xcontent_extract.py`; `default.xex` is extracted first and checked
   (title `4D5309C9`, version 0.0.0.7, entry point `0x82A6FBC0` = the base version, module hash
   `D48ABF1704CE5C4A`; a title update (TU4, hash `336A44DC03EC8782`) or another game is
   refused with a clear message before the long copy).
2. **Title Update 4** (recommended: Horizon Rally and the 1000 Club need it; still optional)
   and **DLC** (optional): pickers for the player's own TU4 package (the STFS package under
   `4D5309C9/000B0000`, or a zip holding it) and DLC folder. The DLC folder is listed with the
   packages' names and installed with `tools/install_dlc.py` (to the game's user data folder).
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

Data lives in the game's user folder (`~/.local/share/forza`, `Documents\forza` on Windows,
changeable under Settings > Advanced): `game/` (extracted files), `toolchain/`, `downloads/`,
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
* Windows (untested): portable LLVM (clang-cl, lld-link), CMake, ninja downloaded the same
  way. The MSVC CRT and Windows SDK come from an installed Visual Studio / Build Tools
  (`vswhere`, `vcvars64.bat`) if there is one, else **xwin** on the player's machine after the
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
Defaults differ from the game's in one place: the first run sets `frame_rate = 60`.

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

Not verified: **everything Windows at run time** (the installer was built by CI but never run;
build path, xwin, Visual Studio detection), the `publish` job (tags only), the TU4 step in the
frozen app (PyInstaller bundles `apply_title_update.py` and `lzx_delta.py`; only the source tree ran it).

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

Not done (stopped early): game files extraction and the build in the VM (xwin path, toolchain download,
SDK overlay, codegen, ninja, wall time/peak memory), Play without Vulkan (note: `vulkan-1.dll` is present
in System32 of the VM, the GPU is a VirtIO display), and the CI job. Next steps: fix the Setup page width,
then `--extract` and `--build --title-update <zip> --jobs 4 --accept-microsoft-license` in the VM (watch
Windows path/quoting issues in `builder.msvc_environment`, the xwin splat and the overlay tar).
`ResourceMonitor` reports no peak memory on Windows (it reads /proc); sample the process tree with
PowerShell if the number is wanted.

CI job to add to `release.yml` (not yet validated in Actions; the script itself ran on the VM):

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
