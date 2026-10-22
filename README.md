<p align="center">
  <img src="docs/images/logo.svg" alt="Forza Horizon 1: ReImagined logo" width="160">
</p>

<h1 align="center">Forza Horizon 1: ReImagined</h1>

> [!TIP]
> **The launcher is out: [latest release](https://github.com/Dyotson/ForzaHorizon1-ReImagined/releases/latest)**
> (Linux Flatpak for any distro, Windows installer; both experimental). It builds the game
> from your own copy, no build steps needed. [Announcement](https://github.com/Dyotson/ForzaHorizon1-ReImagined/discussions/21)
> · [Website](https://forza.dyotson.dev) · [Discord](https://discord.gg/yu7zScdcWw)

> [!IMPORTANT]
> **macOS users: this is the first experimental macOS version.** Native Apple Silicon
> support currently requires a source build; the launcher does not yet support macOS.
> Compatibility and performance are still being validated, including full races,
> save/reload and physical controllers. [macOS setup and known limits](docs/macos.md)

<p align="center">
  The original 2012 Forza Horizon, running natively on PC and rebuilt around the modern
  Horizon experience: a shared online open world, leaderboards and rivals, high frame rates.
</p>

**Forza Horizon 1: ReImagined** takes **Forza Horizon (Xbox 360)** and brings it closer to
what Horizon is today. The game runs natively on Linux and Windows, made by statically
recompiling the original PowerPC executable to C++ with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (the approach of
[RecompiledHaloReach](https://github.com/Dyotson/RecompiledHaloReach)), and on top of that
the project focuses on:

- **Better gameplay:** 60, 120 or 144 fps with the physics and race timing intact, native
  controllers or keyboard and mouse, saves, Title Update 4 with Horizon Rally and the 1000
  Club, and all 23 DLC packages.
- **Online free roam:** the **Horizon World** playlist drops every online player into one
  shared Colorado, where you drive, meet and race together; the goal is an MMO-like world
  where events fill up with players and the grid's free slots with AI
  ([`docs/online_vision.md`](docs/online_vision.md)).
- **Leaderboards and rivals:** times, ghosts and bounties shared between players, Monthly
  Rivals, car clubs and the Message Center, all live again.
- **Your own servers:** Xbox LIVE and Turn 10's services are long gone, so the project
  ships the **Forza Live** server (`server/`), self-hostable, with a public instance to
  join.

This repository contains **no game code and no game data**. You need your own copy of the
game (the Games on Demand package or the disc 1 ISO of the base version, Title ID `4D5309C9`). The recompiled
C++ is generated from your copy on your machine; no binaries are published here.

## Contents

- [Screenshots](#screenshots)
- [What works](#what-works)
- [Known issues](#known-issues)
- [Requirements](#requirements)
- [Install with the launcher](#install-with-the-launcher)
- [Building (Linux)](#building-linux)
- [Windows](#windows)
- [macOS experiment](#macos-experiment)
- [Playing](#playing)
  - [Playing online](#playing-online)
- [More documentation](#more-documentation)
- [Roadmap](#roadmap)
- [Community](#community)
- [Legal](#legal)

## Screenshots

Captured from the recompiled game running natively on Linux.

| | |
|---|---|
| ![Free roam](docs/images/forza-free-roam.jpg)<br>Free roam | ![Circuit race](docs/images/forza-race.jpg)<br>Circuit race against 7 AI drivers |
| ![Prologue drive](docs/images/forza-prologue-drive.jpg)<br>Free roam, Colorado mountains | ![Horizon World online](docs/images/forza-horizon-world-online.jpg)<br>Horizon World online playlist: race lobby and races |

## What works

- **The whole game, single player:** the opening drive to the end of the festival has been
  played through on ReImagined: free roam, festival events and races, showcase events,
  audio and saves. Saves from Xenia Canary can be continued (`tools/play.sh --xenia`).
- **Title Update 4 (the default), Horizon Rally and the 1000 Club:** TU4 is built from your
  own copy of the update (`tools/apply_title_update.py`, about 2 s). It adds the **Horizon
  Rally** expansion (with a Xenia save that has reached it: the Rally Heats and whole
  championships played to the end and saved; the co-driver is not heard yet) and the
  **1000 Club** in the pause menu (signed in to the Forza Live server: its challenges
  earn medals that are kept with the career, and the server has the club's leaderboard).
  [`docs/dlc.md`](docs/dlc.md)
- **All 23 DLC packages:** every DLC car counts as installed, the Rally Expansion's 15 and
  the 1000 Club's cars included (`tools/install_dlc.py`).
- **Keyboard and mouse or controller:** `mnk_mode = true` plays the menus, free roam, a
  race from sign-up to the finish, the pause menu, the map and photo mode with the
  keyboard (WASD, rebindable keys, a steering ramp and the mouse as the right stick).
  Throttle and brake are on/off. Any connected controller drives player 1.
  [`docs/playing.md`](docs/playing.md#keyboard-and-mouse)
- **60, 120 or 144 fps** (`frame_rate` in `forza.toml`, any rate up to 240), with physics, the
  race clock and online play keeping their timing. Measured on a Ryzen 7 7800X3D with a
  Radeon RX 9060 XT ([`docs/framerate.md`](docs/framerate.md), [`docs/perf.md`](docs/perf.md)):

  | `frame_rate` | Free roam | Race, 7 AI cars (Clear Springs Circuit) |
  |---|---|---|
  | 60 | 59.9 fps | 59.6 fps |
  | 120 | 115.2 fps | 75.7 fps |
  | 144 | 111.5 fps | 70.1 fps |

  The race is bound by the GPU emulation above 60; the game itself ran its main loop at
  the target rate in every run. 0 to 100 km/h takes 2.4 s at every rate.
- **Online** (through the Forza Live server): sign-in as a Gold member, Find Game and races
  with XP and credits, parties and custom games, a shared online free roam (the **Horizon
  World** playlist: 8 players per world, JUMP IN while a world runs, the next player
  placed with you when the host leaves), Playground Games modes (Infected, Cat & Mouse,
  King), leaderboards and Rivals (times, ghosts and bounties shared between players), car
  clubs and the club garage, and the Message Center (beaten-time alerts, gifts, club
  requests, invitations, news). Details in [`docs/online_vision.md`](docs/online_vision.md)
  and [`docs/title_services.md`](docs/title_services.md).
- **Title Update 4 online** (tested with two players on a local server): Horizon World (one
  player placed into the other's world), a 30 s event lobby filled with CPU drivers (2
  humans and 6 CPU in an 8-car race) with scores, XP and credits, head-to-head challenges,
  the Rivals screen, the 1000 Club and host migration. Base-game and TU4 players are kept in
  separate rooms, since the two versions cannot play together
  ([`docs/dlc.md`](docs/dlc.md#online-20)).
- **Your own server:** the Forza Live server (`server/`, Python 3 standard library only, or
  Docker) is self-hostable ([`docs/hosting.md`](docs/hosting.md)), and there is a public
  instance to join.

## Known issues

- The Auction House exists on the server only; its screens were removed from the game, and
  the storefront answers empty.
- Throttle, brake and steering on the keyboard are all or nothing (the steering has a ramp),
  and the game's menus have no pointer, so the mouse does nothing there
  ([`docs/playing.md`](docs/playing.md#keyboard-and-mouse)).
- Races with many AI cars are GPU-bound: 76 fps at a 120 fps target and 70 at 144 (60 holds).
- Windows builds are cross-compiled from Linux and have only been tried under Proton, not
  on Windows itself; the launcher's Windows path and installer have never run on Windows.
- The launcher (v0.1.0) is experimental: tested end to end on Linux (Flatpak), not yet on Windows.
- Title Update 4 and base-game players can't play together (separate rooms), and a profile
  that ran on TU4 is refused by the base build.

## Requirements

- Linux x86-64 or Windows 10/11 64-bit, with a Vulkan 1.3 GPU, about 4 GB of free RAM (measured:
  the game peaks at 1.1 GB from the main menu through free roam; the build peaks near 2 GB at
  `-j4` and 3.2 GB at `-j8`, and is not run together with the game) and about 15 GB of disk (7.6 GB of extracted game
  files, the SDK 1.6 GB, the build tree about 1 GB, a downloaded compiler 1.1 GB). Windows:
  see below.
- Your own copy of Forza Horizon, base version: the Games on Demand package
  (`4D5309C9/00007000/<name>` plus `<name>.data/`), or the retail **disc 1 as an ISO** (or a
  folder with its files; see [docs/disc.md](docs/disc.md)). Optional but recommended: the Title Update 4 package
  (`4D5309C9/000B0000/<name>`, update 4000D145) for Horizon Rally and the 1000 Club.
- For the manual build: Clang (tested with 23), CMake >= 3.25, Ninja, Python 3, Git,
  OpenSSL (for `tools/xex_image.py` only). The launcher downloads what is missing.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) 0.10.0.24 nightly
  (`nightly-20261002-bd833a2a`).

## Install with the launcher

This is the way to install. The launcher builds the game on
your machine from your own files; nothing of the game is ever downloaded or shipped.

1. Get the launcher from the [latest release](https://github.com/Dyotson/ForzaHorizon1-ReImagined/releases/latest):
   on Linux a **Flatpak** (works on Bazzite, SteamOS, Chimera, Ubuntu, Fedora and Arch; a
   `.tar.gz` is the alternative), on Windows an **installer**. The Flatpak is installed with
   `flatpak install --user ForzaHorizon1-ReImagined-x86_64.flatpak` (it pulls the freedesktop
   SDK runtime from Flathub by itself); install the compiler extension once with
   `flatpak install flathub org.freedesktop.Sdk.Extension.llvm22//26.08` (0.5 GB; without it the
   launcher downloads a 2 GB compiler instead and tells you so).
2. In **Setup**, point it at your Games on Demand package or your disc 1 ISO (it is
   extracted and checked first), and optionally at your Title Update 4 package and DLC folder.
3. **Build**: it generates the game from your copy and compiles it, downloading a compiler
   and the SDK when you have none. Measured on 8 cores at `-j4`: a full build with Title
   Update 4 takes about 3.6 to 4.6 minutes, peaks near 2 GB of RAM and leaves about 15 GB on
   disk. It can be cancelled and resumed, and is repeated only when something changed.
4. **Play**. Every setting of `forza.toml` has a page in **Settings** (frame rate, display,
   keyboard and mouse bindings, online server, profile, Xenia save import).

Details: [`docs/launcher.md`](docs/launcher.md). The steps below are the same build by hand.

## Building (Linux)

```sh
# 1. The ReXGlue SDK nightly, unpacked to ~/rexglue-sdk-nightly/0.10.0.24/linux-amd64
# 2. The game files, extracted from your Games on Demand package (only read)
python3 tools/xcontent_extract.py /path/to/4D5309C9/00007000/<name> extract extracted/game
# 3. The patched Xenos GPU plugin and codegen tool (clones the SDK to ~/rexglue-sdk-src-forza,
#    applies patches/rexglue-sdk/ and installs an overlay at ~/rexglue-sdk-patched-forza;
#    the stock rexglue's dot products overflow to infinity, Xenon's to QNaN, and free roam
#    crashes without it)
tools/build_rexglue_sdk.sh
# 4. Configure (the first configure runs codegen with the overlay's rexglue, about a
#    minute) and build
cmake -S forza-recomp -B forza-recomp/out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64
ninja -C forza-recomp/out/build/linux
```

**Title Update 4** (needed for Horizon Rally and the 1000 Club; recommended): apply the
update to your `default.xex` (about 2 s; it writes the patched executable, the two facade
DLLs and the update's `media.zip` to `extracted/tu4/`) and build the TU4 version next to the
base one. It has its own hints (`hints_tu4/`) and generated code (`generated_tu4/`); a full
build is about 4 minutes at `-j4` on 8 cores. `git checkout forza-recomp/generated/rexglue.cmake`
afterwards if codegen shows it modified.

```sh
python3 tools/apply_title_update.py extracted/game/default.xex /path/to/<title update package or zip> extracted/tu4/default.xex
cmake -S forza-recomp -B forza-recomp/out/build/linux-tu4 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64 \
  -DFORZA_TITLE_UPDATE=4
ninja -C forza-recomp/out/build/linux-tu4
```

The launcher does all of this from the two packages ([above](#install-with-the-launcher)).

## Windows

The Windows version is cross-compiled from Linux with clang/clang-cl and lld-link, after the
Linux build above (its codegen output in `forza-recomp/generated/` is reused, and step 3
checks out the SDK source). `tools/build_windows.sh` builds the ReXGlue SDK for Windows
from `~/rexglue-sdk-src-forza` with this project's patches and the Vulkan backend into
`~/rexglue-sdk-patched-forza/0.10.0.24/win-amd64`, then the game, and packages it in
`forza-recomp/out/dist/windows` (`--sdk-only` and `--game-only` split the long build):

```sh
brew install llvm lld                                  # clang, clang-cl, lld-link
xwin --accept-license splat --output ~/.local/opt/xwin/sdk   # MSVC CRT + Windows SDK (Microsoft's license)
tools/build_windows.sh
tools/package_linux.sh                                 # the Linux package, forza-recomp/out/dist/linux
```

On Windows, extract the package, install the Microsoft Visual C++ 2015-2022 x64
redistributable, extract your game files with `python xcontent_extract.py` and run
`play.bat C:\path\to\extracted\game` (`--offline` for no Xbox LIVE; it starts a local
Forza Live server when Python is installed, or uses `FORZA_SERVER` / `live_server` in
`forza.toml`). Saves, the online identity and the log go to `Documents\forza`.

## macOS experiment

**This is the first experimental macOS version, for Apple Silicon Macs.**

An experimental native Apple Silicon build path is available through
`tools/build_macos.sh`. Both base-game and TU4 builds boot on macOS. SDK patch 0080 fixes the black
3D world; both opening drives now render and respond to driving input. Following a
comparison with Pinyon Shift, patch 0082 restores solid vehicle stripes with normal
mip filtering. This remains experimental and needs broader gameplay validation. See [`docs/macos.md`](docs/macos.md) for
setup, checks that do not need game data, an isolated offline launch script, and
`tools/package_macos.sh` for a local app with native fullscreen and Game Mode support.
Patch 0084 adds experimental native MetalFX temporal/spatial reconstruction with a
1440p HUD; use `tools/play_macos.sh --tu4 --skip-intro --metalfx` or package with
`tools/package_macos.sh --tu4 --metalfx`. The responsive 720p preset remains faster.
`--balanced` selects 720p with two queued GPU frames; the tested TU4 opening scene
held approximately 60 fps, with more queuing than the one-frame responsive preset.
`--max-fps` retains 720p with two queued frames, raises the ceiling to 240 fps and
uses one FSR presentation pass. The M4 Pro opening-drive sample averaged about
66 fps; the ceiling is not a performance guarantee. Both play and package scripts
accept the preset, with a separate app/profile for the packaged version.
That preset renders the world and HUD at 720p. Native 1440p measured about
30 fps on this M4 Pro. Patch 0085 improves MetalFX's scene/HUD copies; paired
1440p-output samples rose from 40–42 to 49–55 fps, with a 720p world and native
1440p HUD. Steady 1440p/60 still needs verification; see the
[1440p target and exact resolutions](docs/macos.md#1440p-at-60-fps-target).
`--1440p-performance` selects MetalFX Spatial with that native HUD and releases
guest VSync pacing. Its background opening-scene sample averaged 59.1 fps;
a fresh unlocked fullscreen sample averaged 53.8 fps with Simulator also
active. Native 1440p measured 26.7 fps in that session. Steady 60 fps remains
unachieved.
The launcher does not yet support macOS.

## Playing

```sh
tools/play.sh                 # play with your own profile (~/.local/share/forza)
tools/play.sh --xenia         # continue your Xenia Canary save (a copy)
tools/play.sh --offline       # without Xbox LIVE
tools/play.sh --base          # the base game instead of Title Update 4
```

`play.sh` runs the Title Update 4 build (`forza-recomp/out/build/linux-tu4`) when it exists
and the base build otherwise. TU4 converts a profile save when it loads it
(`ForzaProfile/VersionFlags`, a `NeedsAA` file), and the base build then refuses that
profile ("MISSING TITLE UPDATE"): `--base` is for profiles that never ran on TU4, and a copy
of the profile folder lets you go back.

Install DLC once with `python3 tools/install_dlc.py /path/to/4D5309C9/00000002` (STFS packages, or a Xenia `content` folder with the DLC extracted; the launcher's DLC step takes both). Controllers,
saves, the Xenia save import and more are in [`docs/playing.md`](docs/playing.md).

### Playing online

`play.sh` starts a local Forza Live server for the session. To play on the public server,
set in `forza.toml` next to the binary:

```toml
live_server = "forza-reimagined.playit.plus:56677"
```

(or start with `FORZA_SERVER=forza-reimagined.playit.plus:56677`). You forward no ports. To run your
own server, see [`docs/hosting.md`](docs/hosting.md).

## More documentation

[`docs/PROJECT.md`](docs/PROJECT.md) (status log, findings, debugging recipes),
[`docs/playing.md`](docs/playing.md), [`docs/hosting.md`](docs/hosting.md),
[`docs/perf.md`](docs/perf.md), [`docs/framerate.md`](docs/framerate.md),
and the [issue tracker](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues)
(known bugs and planned work).

## Roadmap

Where the project is heading, on the [ReImagined Roadmap](https://github.com/users/Dyotson/projects/3) board:

1. **Stable everywhere**: the game runs well on any PC, GPU and the Steam Deck.
2. **Multiplayer as cool as possible**: Horizon World, challenges, ranked, clubs, sharing tunes and liveries.
3. **Modding, and more multiplayer**: a mod loader, car and tuning mods, custom events, mod sync online.
4. **Decompilation and custom game**: readable C++ game systems, new modes, decoupled physics, better AI.

Every card is an issue: comment there to discuss an item or help with it.

## Community

Questions, help with setup and online sessions: [Discord](https://discord.gg/yu7zScdcWw).
Bugs and feature requests: [issues](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues)
(the bug report template asks for your OS, GPU and log).

## Legal

Forza Horizon is © Microsoft. This project is not affiliated with, endorsed by or
sponsored by Microsoft, Playground Games or Turn 10 Studios. It contains no game code or
assets; you must own the game. See [`LICENSE`](LICENSE).
