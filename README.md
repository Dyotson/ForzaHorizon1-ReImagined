<p align="center">
  <img src="docs/images/logo.svg" alt="Forza Horizon 1: ReImagined logo" width="160">
</p>

<h1 align="center">Forza Horizon 1: ReImagined</h1>

> [!TIP]
> **Install with the launcher: [latest release](https://github.com/Dyotson/ForzaHorizon1-ReImagined/releases/latest).**
> Windows installer, Linux Flatpak (recommended on SteamOS), or the Linux `.tar.gz`.
> It updates itself, and it builds the game on your PC from your own copy.
> [Website](https://forza.dyotson.dev) · [Discord](https://discord.gg/yu7zScdcWw)

> [!IMPORTANT]
> **macOS is a source-build experiment.** The launcher does not support macOS, and there is no Mac download.
> Opening drives render on Apple Silicon. Full races, save reloads and physical controllers still need validation.
> [macOS setup and limits](docs/macos.md)

<p align="center">
  The original 2012 Forza Horizon, running natively on PC: single player, online free roam,
  leaderboards and rivals. Windows is what most players use now. Linux works too.
</p>

**Forza Horizon 1: ReImagined** runs **Forza Horizon (Xbox 360)** natively on Windows and Linux.
The original PowerPC executable is statically recompiled to C++ with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the same approach as
[RecompiledHaloReach](https://github.com/Dyotson/RecompiledHaloReach). On top of that build the project adds:

- **The whole festival, single player.** Free roam, festival events, showcase events, audio and saves have been played through on ReImagined. A Xenia Canary save can be continued. Title Update 4 (the default) adds Horizon Rally and the 1000 Club, and all 23 DLC packages can be installed. The Rally co-driver is heard. [`docs/dlc.md`](docs/dlc.md)
- **Online through Forza Live.** Xbox LIVE and Turn 10's services are gone. The game signs in to the Forza Live server (`server/`): Horizon World, the friends list and chat, leaderboards and Rivals, car clubs and the Message Center. There is a public server, and you can host your own. [`docs/online_vision.md`](docs/online_vision.md)
- **A frame rate you set.** The original game is 30 fps. The setting goes up to 240 for a PC that can hold the higher rate. The high numbers further down were measured on one desktop. A Steam Deck or a low-end PC is much slower.

This repository contains **no game code and no game data**. You need your own copy of the
game (a disc image, a Games on Demand package, or an extracted folder). The recompiled
C++ is generated from that copy on your machine. No game binaries are published here.

## Contents

- [Screenshots](#screenshots)
- [What you can play](#what-you-can-play)
- [Performance](#performance)
- [Known issues](#known-issues)
- [Requirements](#requirements)
- [Install with the launcher](#install-with-the-launcher)
- [Building from source](#building-from-source)
- [Playing](#playing)
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

## What you can play

- **Single player, Title Update 4 and DLC.** TU4 is the default when you give the launcher your update. Horizon Rally and the 1000 Club need it. All 23 DLC packages count as installed, including the Rally Expansion's cars and the 1000 Club's cars. [`docs/dlc.md`](docs/dlc.md)
- **Controls.** Keyboard and mouse (menus, free roam, a race, the pause menu, the map and photo mode). Throttle and brake are on/off, and the menus have no pointer. [`docs/playing.md`](docs/playing.md#keyboard-and-mouse). A gamepad works. Racing wheels are presented as the Xbox 360 Wireless Racing Wheel, with the game's force feedback, an H-pattern shifter and a clutch pedal. Logitech G29, G920 and G923 are tested; other wheels go through **Settings > Controls > Axis numbers** and **Settings > Controls > Button numbers**. Press-to-bind mapping and calibration are in progress ([#279](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/279)). On the launcher's Controls page, **DualSense adaptive triggers** (**Enable adaptive triggers while driving**) adds throttle and brake resistance on a DualSense or DualSense Edge. It follows how far the trigger is pressed. RPM, tire slip, ABS and road effects are not available yet. [`docs/playing.md`](docs/playing.md#racing-wheels-issue-4)
- **Picture.** **Settings > Display > Upscaling filter:** Bilinear, AMD CAS (sharpen), AMD FSR 1, or AMD FSR 3 (temporal), on any GPU under Vulkan. On Windows, an RTX GPU can use **Settings > Display > NVIDIA DLSS** (Quality, Balanced, Performance) and DLAA. **Settings > Display > Anti-aliasing:** the game's MSAA, SMAA 1x, FSR Native AA, and on Windows NVIDIA DLAA. A separate anti-aliasing method applies when upscaling is off. **Settings > Display > Render scale** draws at 1x to 8x the game's 1280x720. **Settings > Display > Frame rate** lists 20, 30, 40, 60, 75, 90, 100, 120, 144, 165, 180, 200 and 240 fps, or a rate you type from 60 to 240. 20 is for a PC that can't hold 30: every frame stays on a 60 Hz screen equally long. The first entry is **Match display**, which follows the monitor the game runs on, up to what that PC is likely to hold. **Settings > Display > HDR output** is Vulkan only: an HDR display with HDR turned on in the OS. It brightens the existing SDR picture. It does not add HDR detail the game never stored, and Direct3D 12 does not use it. **Settings > Display > Ultrawide gameplay** widens the view under Vulkan. Menus and the map stay 16:9. Title Update 4 also has a Graphics page in the pause menu. [`docs/rendering.md`](docs/rendering.md) · [`docs/hdr.md`](docs/hdr.md) · [`docs/ultrawide.md`](docs/ultrawide.md) · [`docs/antialiasing.md`](docs/antialiasing.md)
- **Photos, the minimap, unicorn cars.** Photo mode saves into your profile. The launcher's Photo gallery browses those pictures and can export a PNG or JPEG. The minimap uses the full-size map icons. **Saves** backs up the profile before Play (on by default) and can restore a backup. **Edit profile…** can change credits, XP and the wristband offline, and **Add missing unicorns** adds the seven unicorn cars the save does not already have. [`docs/unicorn_cars.md`](docs/unicorn_cars.md)
- **Crash reports.** After an unexpected exit, **Play > Review crash report…** shows a report you can edit, copy or save. Nothing is uploaded unless you choose to open a GitHub issue.
- **Online.** See [Playing online](#playing-online).

## Performance

The game's original cap is 30 fps. On a low-end PC or a Steam Deck, leave it there. **Settings > Display > Frame rate** at 60 or higher is for a PC that can hold it.

These numbers are from one machine, a Ryzen 7 7800X3D with a Radeon RX 9060 XT ([`docs/framerate.md`](docs/framerate.md), [`docs/perf.md`](docs/perf.md)). 0 to 100 km/h took about 2.4 seconds at every rate on that PC. Above 60 fps the race is bound by the GPU emulation there; the game's main loop still ran at the target rate.

| `frame_rate` | Free roam | Race, 7 AI cars (Clear Springs Circuit) |
|---|---|---|
| 60 | 59.9 fps | 59.6 fps |
| 120 | 115.2 fps | 75.7 fps |
| 144 | 111.5 fps | 70.1 fps |

Steam Deck players report about 15–20 fps in free roam, and less in a big race ([#45](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/45), [#140](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/140)). A Deck test of 0.1.27 was about 17 fps in free roam and about 8 fps in an 8-car race. 4-core laptops and integrated GPUs report about 15–25 fps ([#263](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/263), [#268](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/268)). The launcher's Deck preset is Vulkan, render scale 1, AMD FSR 1 and 30 fps. Higher render scales and 60 fps cost more than the Deck has to spare.

Shader prebuild and the shader-cache fixes shipped in 0.1.28. Further CPU savings for 4-core systems are in testing ([#45](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/45)). [`docs/shader_prebuild.md`](docs/shader_prebuild.md) · [`docs/perf.md`](docs/perf.md)

## Known issues

- **Low-end CPUs and the Steam Deck.** Free roam is about 15–20 fps on a Deck and about 15–25 fps on a 4-core laptop or an integrated GPU. Big races are slower. Use 30 fps. Work is in progress ([#45](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/45), [#140](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/140), [#263](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/263), [#268](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/268)).
- **NVIDIA.** Stutter on Vulkan with a warm shader cache ([#49](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/49), [#158](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/158)), green texture glitches ([#106](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/106)), flicker and coloured artifacts at render scale 3 ([#47](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/47)), and flicker and greenish reflections on Direct3D 12 ([#46](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/46)). On Windows, **Settings > Display > Graphics API** should stay on Vulkan unless you are comparing. Some fast NVIDIA PCs also report the game barely using the machine ([#181](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/181), [#34](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/34)).
- **Crashes.** A bad guest read during a race is still open ([#83](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/83)). Quitting to the title screen can crash ([#262](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/262)). Alt-tab on some NVIDIA systems aborts the game ([#221](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/221)). A race crash that exited with code 134 ([#182](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/182)) is fixed in 0.1.28; if you still hit it, comment there. Flashing or black video is still open on some AMD and Intel machines ([#91](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/91), [#114](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/114)).
- **Title Update 4 and the base game do not share rooms.** A TU4 game only meets other TU4 players. A profile that has been saved on TU4 is refused by the base game ("MISSING TITLE UPDATE"). [`docs/dlc.md`](docs/dlc.md#online-20)

Other open reports include a lobby that stops taking joins after time in a club ([#186](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/186)) and an online level reset ([#245](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues/245)).

## Requirements

- Windows 10/11 64-bit, or Linux x86-64. Windows can use Vulkan or Direct3D 12. Linux uses Vulkan. Vulkan 1.3 is what the Vulkan path asks for.
- About 4 GB of free RAM while playing (the game peaks near 1.1 GB). A build needs more: about 2 GB at 4 jobs, about 3.2 GB at 8. About 15 GB of disk. [`docs/launcher.md`](docs/launcher.md)
- Your own copy of Forza Horizon: a disc image, a Games on Demand package, or an extracted folder. Title Update 4 and DLC are optional. TU4 is what you want for Horizon Rally and the 1000 Club.
- A controller, a keyboard and mouse, or a wheel.

## Install with the launcher

This is the way to install. The launcher builds the game on your machine from your own files. Nothing of the game is downloaded.

1. Get the launcher from the [latest release](https://github.com/Dyotson/ForzaHorizon1-ReImagined/releases/latest). On Windows, the installer. On Linux, the Flatpak (recommended on SteamOS) or the `.tar.gz`. From v0.1.9 onward the launcher checks for updates itself (**About > Launcher updates**).
   The Flatpak is `flatpak install --user ForzaHorizon1-ReImagined-x86_64.flatpak`. It pulls the freedesktop SDK runtime from Flathub. Install the compiler extension once, or the launcher downloads a 2 GB compiler instead and tells you so:

   ```sh
   flatpak install flathub org.freedesktop.Sdk.Extension.llvm22//26.08
   ```

2. In Setup, **Game files**: your disc image, Games on Demand package, or an extracted folder. **Updates & DLC**: Title Update 4 and your DLC folder, if you have them.
3. **Build.** The launcher generates the game from your copy and compiles it. A measured build at 4 jobs on an 8-core PC took about 4.6 minutes ([`docs/launcher.md`](docs/launcher.md)). It can be cancelled and resumed, and it runs again only when something changed.
4. **Prebuild shaders** (optional, Vulkan). This builds pipelines from your own game files before you play, so the first visit to a place stutters less. The launcher offers it again after a driver, launcher or graphics-setting change. [`docs/shader_prebuild.md`](docs/shader_prebuild.md)
5. **Play.** Settings cover display, controls, audio, online, profile and advanced options. **Saves** is the backup and profile editor.

Details: [`docs/launcher.md`](docs/launcher.md).

## Building from source

For developers. Players should use the launcher.

Linux configure and the pinned SDK are in [`docs/PROJECT.md`](docs/PROJECT.md). Title Update 4 is in [`docs/dlc.md`](docs/dlc.md). The Windows cross-compile is `tools/build_windows.sh` ([`docs/launcher.md`](docs/launcher.md)). macOS is [`docs/macos.md`](docs/macos.md). A source tree launches with `tools/play.sh` ([`docs/playing.md`](docs/playing.md)).

## Playing

Use the launcher's Play button. It signs in to the Forza Live server unless you pick **Offline**.

To keep playing a Xenia Canary career, choose **Settings > Profile > Continue a Xenia Canary save**. The launcher copies that save once. The original Xenia files are not modified. [`docs/playing.md`](docs/playing.md)

### Playing online

In **Settings > Online**, pick one:

- **Public server.** Horizon World, leaderboards and rivals. The address filled in is `forza-reimagined.playit.plus:56677`. You forward no ports.
- **This PC.** A private server for this session.
- **My own server.** [`docs/hosting.md`](docs/hosting.md). Python 3 from the standard library, or Docker.
- **Offline.** No Xbox LIVE.

Horizon World is the first playlist under Multiplayer, Find Game. The server places you in the fullest open world, up to 8 players, or you open one and drive. Someone who joins later gets JUMP IN. When the host leaves, the others keep driving. Each world is its own session. A local test has run with three players. [`docs/online_vision.md`](docs/online_vision.md)

Friends (F8, or hold the left stick on a controller) and chat (F9) use the server's list. Leaderboards cover speed traps, speed zones, events and Rivals (times, ghosts and bounties), and the public server's are on the [website](https://forza.dyotson.dev/leaderboards.html). Car clubs and the Message Center work. The club garage can share a car. [`docs/title_services.md`](docs/title_services.md) · [`docs/playing.md`](docs/playing.md)

Title Update 4 players and base-game players do not meet. The Auction House has no screens in this game. The server can store auctions, and there is nowhere in the game to open them.

## More documentation

[`docs/launcher.md`](docs/launcher.md),
[`docs/playing.md`](docs/playing.md),
[`docs/hosting.md`](docs/hosting.md),
[`docs/perf.md`](docs/perf.md),
[`docs/framerate.md`](docs/framerate.md),
[`docs/online_vision.md`](docs/online_vision.md),
[`docs/PROJECT.md`](docs/PROJECT.md),
and the [issue tracker](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues).

## Roadmap

Where the project is heading, on the [ReImagined Roadmap](https://github.com/users/Dyotson/projects/3):

1. **Stable everywhere** (in progress): the game runs well on any PC, GPU and the Steam Deck.
2. **Multiplayer as cool as possible:** Horizon World, challenges, ranked, clubs, sharing tunes and liveries.
3. **Modding, and more multiplayer:** a mod loader, car and tuning mods, custom events, mod sync online.
4. **Decompilation and custom game:** readable C++ game systems, new modes, decoupled physics, better AI.

Every card is an issue. Comment there to discuss an item or help with it.

## Community

Questions, help with setup and online sessions: [Discord](https://discord.gg/yu7zScdcWw).
Bugs and feature requests: [issues](https://github.com/Dyotson/ForzaHorizon1-ReImagined/issues).
The bug report template asks for your OS, CPU, GPU and log.

## Legal

Forza Horizon is © Microsoft. This project is not affiliated with, endorsed by or
sponsored by Microsoft, Playground Games or Turn 10 Studios. Forza, Forza Horizon, Xbox and Xbox LIVE are trademarks of Microsoft. It contains no game code or
assets; you must own the game. See [`LICENSE`](LICENSE).
