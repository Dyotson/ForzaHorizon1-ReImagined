# Forza Horizon 1: ReImagined: native static recompilation of Forza Horizon (Xbox 360)

Goal: run Forza Horizon (Xbox 360, Title ID `4D5309C9`, base version, no title update)
natively on x86-64 Linux (later Windows) by statically recompiling `default.xex` and its
facade DLLs (PowerPC Xenon) to C++ with the ReXGlue SDK. The approach, tooling and many
runtime fixes come from [RecompiledHaloReach](https://github.com/Dyotson/RecompiledHaloReach)
(same SDK, same author); files taken from it say so.

## The game files

| | |
| --- | --- |
| Container | Games on Demand (content type `0x7000`), SVOD: a header file `4D5309C9/00007000/589C7B8B0AE2A7A0CBE0` plus 46 fragments in `589C7B8B0AE2A7A0CBE0.data/` (7.6 GB), no XSF header, XDVDFS magic at `0x12000` of `Data0000` |
| `default.xex` | Base version (Xenia module hash `D48ABF1704CE5C4A`, matching `patches/4D5309C9 - Forza Horizon.patch.toml`; TU4 would be `336A44DC03EC8782`). Version 0.0.0.7, built 2012-09-04, Media ID `2B7A1346` (Europe/Asia disc), 20.9 MB, load address `0x82000000`, image size `0x1620000`, entry point `0x82A6FBC0`, AES-encrypted with basic (zero-run) compression |
| DLLs | `XMediaFacade_default.xex` (loads at `0x88000000`) and `SpeechFacade_default.xex` (`0x89000000`), loaded at run time with `XexLoadImage` |
| Data | `media/` (cars, tracks, audio, shaders, UI zips, string tables...), `nuisp*` Kinect speech models, `$SystemUpdate/` (dashboard update, not used) |
| DLC | `4D5309C9/00000002/`: 23 LIVE (STFS, read-only format) marketplace packages, content type 2: car packs, VIP, Season Pass, Rally Expansion (1 GB), 1000 Club Expansion. Each holds `Media/<id>.puboffer` and `Media/DLCZips/<id>_pri_65[.zip]` (the expansions also `spa.bin`). Not used yet |

`tools/xcontent_extract.py` (standard library only, read-only) lists and extracts both
container kinds; it is a port of the SDK's `StfsContainerDevice` (from Xenia):

```sh
G=~/forza-horizon-.../4D5309C9
python3 tools/xcontent_extract.py $G/00007000/589C7B8B0AE2A7A0CBE0 info
python3 tools/xcontent_extract.py $G/00007000/589C7B8B0AE2A7A0CBE0 extract extracted/game
python3 tools/xcontent_extract.py $G/00000002/<package> list
```

The extraction was checked against the SVOD level-0 hash tables (SHA-1 of every 4 KB
data pair in all 46 fragments: no mismatch). SVOD block mapping: each 0xA290000-byte
fragment is one level-1 hash block, then 0xCB groups of (level-0 hash block + 0x198
data sectors of 0x800 bytes).

`tools/xex_image.py XEX OUT --elf` writes the decrypted, decompressed image as a PowerPC
ELF (via `openssl enc` for AES), and `tools/ppcdis.sh ADDR [+LEN]` disassembles it with
`llvm-objdump` (VMX128 shows as `<unknown>`). Quicker than Ghidra for looking at a few
instructions; both read `extracted/analysis/`.

## Layout

| Path | What |
| --- | --- |
| `extracted/game/` | Extracted game (gitignored, never committed) |
| `extracted/analysis/` | Decoded xex images for disassembly (gitignored) |
| `forza-recomp/forza_manifest.toml` | Codegen manifest (`default.xex` + 2 DLL modules) |
| `forza-recomp/hints/` | Function boundaries, rexcrt mappings, per-module hints |
| `forza-recomp/src/` | Runtime code: app class, kernel/XAM overrides, debug tools |
| `forza-recomp/generated/` | Codegen output; only `rexglue.cmake` is tracked here, the rest goes to the private `RecompiledForzaHorizon-generated` repo (`tools/sync_generated_repo.sh`) |
| `patches/rexglue-sdk/` | SDK patches (started as Reach's 19 GPU patches) |
| `ghidra_projects/ForzaHorizon` | Ghidra project with `default.xex` (gitignored) |

## Tooling

- ReXGlue SDK nightly 0.10.0.24 (`nightly-20261002-bd833a2a`) at
  `~/rexglue-sdk-nightly/0.10.0.24/linux-amd64`.
- Patched Xenos GPU plugin: `tools/build_rexglue_sdk.sh` builds it into this project's own
  overlay `~/rexglue-sdk-patched-forza/0.10.0.24/linux-amd64` from its own clone
  `~/rexglue-sdk-src-forza` (never Reach's `~/rexglue-sdk-src`/`~/rexglue-sdk-patched`).
  Until Forza needs a patch of its own, CMake and the run scripts fall back to Reach's
  overlay, which has the same patch set.
- Clang 23, CMake, Ninja from Homebrew (`/home/linuxbrew/.linuxbrew/bin`).
- Ghidra 12.1.4 + XEXLoaderWV: `ghidra_projects/ForzaHorizon` was made with
  `analyzeHeadless ... -import extracted/game/default.xex` (about 25 minutes with 6 GB heap).

Build (always through the same path, `/var/home/...`; `/home` is a symlink and a
different spelling makes CMake rebuild everything):

```sh
export PATH=/home/linuxbrew/.linuxbrew/bin:$PATH
cmake -S forza-recomp -B forza-recomp/out/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64
nice -n 15 ninja -C forza-recomp/out/build/linux -j4
```

## Pre-release checks

`tools/prerelease.sh [--skip-build] [--only sdk,build,unit,boot,server,launcher,ci] [--iso PATH] [--ci]`
runs them all, non-interactively, in about 25 minutes: one PASS/FAIL line per check, the last 30 lines of a
failing check's log, full logs in `~/.cache/forza-prerelease/<timestamp>/`, exit status = failures.

| Check | What |
|---|---|
| sdk | the SDK overlay matches `patches/rexglue-sdk` (reinstalled by `tools/build_rexglue_sdk.sh` only when the patches changed since the last install; a game running from the overlay is affected) |
| build | ninja of `linux-tu4` and `linux`, then restores `generated/rexglue.cmake` |
| unit | `launcher/test_*.py` and `server/test_*.py` |
| boot | TU4 build to the main menu on a scratch copy of the Xenia save (never `~/.local/share/forza`) with `present_effect` bilinear, cas and fsr, then the base build: no fault, frame rate above 5 fps, a non-flat presenter image that changed after START (the screenshot hook captures the guest output before the upscaling filter, so the filter itself is not seen; a crash or Presenter layout mismatch with the SDK would show) |
| server | a Forza Live server on ports 21310/21311: the game signs in, reaches the menu, its stats reads (XUserReadStats) are answered |
| launcher | the launcher as a frozen build runs it (foreign `LD_LIBRARY_PATH`, #53): AES helper, default.xex and facades out of the disc image (`--iso`, default `HorizonISO/*.iso`) |
| ci | `--ci`: `gh workflow run release.yml` (artifacts only) and wait for it; prints the run id |

Leaderboard screens, Windows and Steam Deck are not scripted: a hand run in the VM / on the device.

## Hard rules

- Never modify, move or delete anything in the game folder, the zip next to it, Xenia
  (`~/xenia_canary_windows`) or the Reach project and its SDK checkout/overlay.
- No game code or data in this repository: `extracted/`, `*.xex`, generated C++ and
  Ghidra projects are gitignored. The generated C++ goes only to the private repo.
- Game runs: under `timeout`, with every SDL pad ignored, after checking `free -g`
  (the run scripts refuse with less than 10 GB available); kill by PID.

## Status log

- 2026-10-10: project started. Game extracted from the GOD container (base version, see
  above). Codegen of `default.xex` + both facade DLLs succeeds: 76,788 functions in
  `default.xex`, 248 MB of C++. Hints needed so far:
  - 20 tail-call-only entry points (accessors, forwarders, this-adjusting thunks placed
    right after an unconditional branch) that failed validation with "target not in any
    function" (`hints/tail_targets.toml`, `hints/xmediafacade.toml`,
    `hints/speechfacade.toml`), found by iterating codegen on its error list.
  - The statically linked XAPI fiber routines (same code as in Reach) mapped to the SDK's
    native fibers (`hints/rexcrt.toml`); without the mapping codegen could not even
    resolve SwitchToFiber's tail call into its context save at `0x830ED910`.
  - Known benign warnings: `bdz` at `0x82AD8138/813C` branch to `0x82AD836C`, the zero
    word just past the function (MSVC's marker for unreachable switch cases); function
    `0x8241A370` is 2.5 MB (one huge initializer).

- 2026-10-10 (later): **boots to gameplay.** The game plays the Microsoft Studios intro
  video, shows the title screen ("PRESS START"), the first-run video calibration screen,
  the main menu (Single Player / Multiplayer), the live-action festival intro video, and
  then the in-engine prologue: the SRT Viper cinematic on the road and the drive to the
  festival with HUD (minimap, speedometer), steerable with scripted pad input. Rendering
  looks right with the patched GPU plugin (Reach's 19 patches). Screenshots in
  `screenshots/` (gitignored). What it took:
  - 220 indirectly referenced entry points (vtables, callback tables, CRT initializers)
    and 357 orphan functions in `default.xex`, plus 37/22 and 89/19 in the two DLLs,
    found with Reach's tools ported to read the XEX image instead of Ghidra
    (`tools/find_missing_entries.py`, `tools/find_orphan_functions.py`). The first boot
    died in `XMediaFacade`'s CRT initializers ("Call to invalid or unregistered function
    at 0x882435D8", a `__xc_a` entry). Six hints that split functions were pruned
    (`tools/prune_split_hints.py` plus two by hand: a jump-table case after a call at
    `0x829CBE14`, an EH funclet at `0x82ABAF38`), and 20 that landed on import thunks
    (codegen: "outranked by import") were dropped: they left undefined `sub_831EDxxx`
    symbols, and in a DLL an undefined symbol only shows up when `dlopen` fails at run
    time ("Failed to load shared library for module").
  - `cache:` mounted on a host folder (`<cache_root>/cache`) in `ForzaApp::OnPreLaunchModule`,
    as Xenia does with `mount_cache`: the game keeps ghost and replay streams there.
  - Codegen rewrites the DLL sources during the build, but only `default.xex`'s sources
    are declared codegen outputs, so ninja picks up DLL changes on its next run:
    `tools/build.sh` runs ninja twice.
  - Open: `Resolve region is empty` + `PM4_DRAW_INDX_2(3, 8, 2): Failed in backend`
    logged ~180 times a second (Xenia later made empty resolves a silent no-op,
    "log spam in Forza Horizon 1/2"); `XamXStudioRequest` stub logged 33 times a second
    (Xenia also gets it, 314k calls in the user's log); audio not verified.

- 2026-10-10 (afternoon): **the opening drive plays at 30 fps with sound.**
  - Own SDK clone and overlay (`~/rexglue-sdk-src-forza`, `~/rexglue-sdk-patched-forza`)
    with patch 0023: an empty resolve region is a no-op, as in Xenia, which changed it
    for Forza Horizon. It was logged twice per resolve, about 180 lines a second
    (`Resolve region is empty` + `PM4_DRAW_INDX_2(3, 8, 2): Failed in backend`).
  - `src/kernel/xam_nui.cpp`: Kinect and XAM answers as in Xenia:
    `XamNuiGetDeviceStatus` returns 0xC0050006 (no Kinect; the SDK returned nothing),
    `XamXStudioRequest` (polled every 30 ms) is left unanswered without a warning per
    call, XAM app 0xFE message 0x2B003 succeeds.
  - Title screen and the drive run at a steady 29.5-30 fps (`FORZA_FPSLOG=1`); the
    drive was held at full throttle for two minutes (89 mph on the road, then into the
    woods) without errors. Audio frames carry sound (`FORZA_AUDIOLOG=1`: RMS 0.01-0.04
    during the intro and menus). Saves: the profile (`ForzaProfile`) is written under
    `~/.local/share/forza/<xuid>/4D5309C9/00000001/`; the second run skips the first-run
    screen.
  - Online vision (design only, for later): `docs/online_vision.md`.

- 2026-10-10 (afternoon, later): **free roam, events and races; DLC; Xenia saves.**
  - DLC: `tools/install_dlc.py` extracts the 23 LIVE packages into the runtime's content
    layout (`<user data>/0000000000000000/4D5309C9/00000002/<package>/` plus a
    `Headers/00000002/<package>.header`), byte-identical to what the user's Xenia
    installed. The game mounts them as `DLCRoot0:`..`DLCRoot22:` after START.
  - Saves are tied to the profile: the user's Xenia save (profile `E030000004B490FA`)
    copied under the runtime's profile folder froze the title screen after START. The
    game checks its save with a keyed hash (`sub_82C66308`, key decoded from an
    obfuscated table at `0x82518CF8`, failure callback `sub_824F31F8`), and the failure
    path calls the fatal-error handler `sub_82C09F00` (records four values at
    `0x83442EBC`, sets `0x83442ED1`, spins in `b .` at `0x82C09FA8`). With
    `profile_xuid=E030000004B490FA` (`src/kernel/profile_identity.cpp`; also
    `FORZA_XUID`, `profile_gamertag`/`FORZA_GAMERTAG`) the save loads: free roam in the
    user's Aventador with traffic, a festival event ticket, car select, the pre-race
    menu and a 4-lap circuit race against 7 AI drivers at 30 fps.
  - `src/kernel/xekeys.cpp`: `XeKeysGetConsoleID`, `XeKeysConsolePrivateKeySign`,
    `XeKeysConsoleSignatureVerification` and `XamUserGetIndexFromXUID` answer as in
    Xenia (the SDK's stubs wrote nothing, or returned FALSE).
  - `src/debug/fault_guard.cpp`: a fault nothing handles now logs a host stack
    (module+offset) and aborts, instead of re-running the faulting instruction forever.
    One run with the copied save hit a null read (`read of guest 0x00000000` on one
    thread, logged 1.6 million times: 260 MB in a minute) 30 s after entering single
    player; the next identical run did not.
  - That fault, chased with a repeat script (copy the Xenia save, START, Single Player,
    watch for 110 s): it hits in roughly half the runs, always while free roam loads
    around the saved position, never in the prologue. Two forms: a read of guest
    `0x0000000F` and a write of guest `0x701F0120`, both under `sub_82D3DB00` (called
    from `sub_82D44B90` <- `sub_82D42638` <- `sub_82D43818` <- `sub_82D4D9D0`/
    `sub_82D4DCC8`), the loader of world physics/collision definitions (strings
    `PhysicsDefinition`, `ConvexHull`, `BreakSpeedMph`...). `sub_82D3DB00` keeps an
    array of at most 8 records of 544 bytes in its 4544-byte frame and fills one per
    entry of a 12-byte record chosen by its index argument (`*(r3+84) + index*12`,
    count byte at +9, list pointer at +4). The bad runs read a count above 8 or a
    pointer of 0xF there: the record is garbage, and the loop writes past the frame,
    through the thread's PCR (`r13` = `0x701EE890`) up to the end of the stack at
    `0x701F0000`. So the input is not ready or not what the game expects when it is
    read; the timing dependence points at synchronization (a race the console's timing
    hides, or an emulated primitive that misbehaves). Ruled out: the critical-section
    replacement (it faults with `FORZA_SDK_CRITICAL_SECTIONS=1` too) and fiber
    migration between threads (`FORZA_FIBERTRACE=1`: the game switches fibers once).
    Fixed later the same day (Xenon dot-product overflow, see below).

- 2026-10-10 (evening): **any controller gets past the title screen; Xenia saves from
  `play.sh`; perf tooling from Reach.**
  - The user was stuck at the title screen: the runtime signs in one profile, on user 0,
    and gives every SDL pad its own user slot in connection order. With Steam Input and
    Sunshine virtual pads present, the user's pad was not slot 0, so START came from an
    unsigned user and the game waited for a sign-in nobody could give (reproduced with
    `FORZA_PAD_PER_USER=1` and a scripted press on slot 1: `PAD1:START` on the input FIFO).
    `src/kernel/input_autopress.cpp` now merges every pad into user 0 (buttons combined,
    strongest stick/trigger wins, vibration to all) and reports users 1-3 as without a
    controller: START on a slot-1 or slot-2 pad reaches the first-run screen and the main
    menu from a fresh data folder.
  - `tools/play.sh --xenia [XUID]` plays a copy of the user's Xenia save signed in with
    its XUID (`~/.local/share/forza/xenia-<XUID>/`, DLC and shader cache shared).
  - Frame statistics, Vulkan hooks (present-mode log, present and pipeline counts), the
    FIFO present-mode default and the on-demand achievement toast (the SDK's toast kept
    the UI thread repainting nonstop) ported from RecompiledHaloReach. SDK patch 0024
    (a presenter hook) is gone again: the Vulkan hooks do it without an SDK change.
  - Diagnostics for the physics fault: the item index that overruns `sub_82D3DB00`'s
    array is the low byte of the w lane of a 16-byte closest-point result
    (`sub_82D44B90` r1+384/+400, written whole by `sub_82D41F28` from GJK vertices with
    `vmaddfp`/`vsel`); in failing runs that lane holds pointers or 0xBEBEBEBE fill, i.e.
    the vertex data's w lanes carry garbage. Next: who writes the vertex records
    (r3+0x1AA0 + id*32 in the solver workspace).
- 2026-10-10 (night): **Xbox LIVE through a Forza Live server; Gold; the party
  lobby.** The user saw "You must have an Xbox LIVE Gold Membership" and "no connection
  to Xbox LIVE" under Multiplayer.
  - RecompiledHaloReach's Reach Live client is ported (`src/kernel/identity.cpp`,
    `xam_signin.cpp`, `net.cpp`, `live_xmsg.cpp`, `live_tcp.cpp`, `live_lsp.cpp`,
    `src/platform/socket.*`) with its server (`server/forza_live_server.py`, UDP 21200 and
    HTTP 21201; Reach Live keeps 21100/21101, and an instance of each game can run side by
    side). `tools/play.sh` starts a local server unless `FORZA_SERVER` or `live_server`
    names one (`--offline`: none). The game signs in to LIVE as user 0 with an online XUID
    (`0009...`, kept in `<data>/forza/4D5309C9/live_identity.txt`) while the save stays
    tied to the offline XUID.
  - Gold, always (`xam_signin.cpp`): `XamUserCheckPrivilege` grants everything,
    `XamUserGetMembershipTier[FromXUID]` is Gold, sign-in state 2 with the LIVE flag.
  - Entering Multiplayer then crashed (read of guest `0xC` in `sub_82CA41C8`). Two causes:
    - `XUserReadStats` (XGI `0xB0021`): the SDK answers success and leaves the results
      empty; the game reads its TrueSkill row (column 0 as an int64, two doubles: mu and
      sigma) through the empty view list right after creating the party session.
      `live_xmsg.cpp` now writes a row per player and view: the server's
      (`server/forza_stats.py`, which also keeps and rates what `XSessionWriteStats`
      writes at the end of an online game; `docs/title_services.md`, "Every
      leaderboard"), or a new player's defaults (skill 1, mu 3.0, sigma 1.0, other columns
      null).
    - The session socket: Forza's session networking (`0x8313xxxx`-`0x8315xxxx`) opens a
      VDP socket, binds it, reads its port with `getsockname` (an SDK stub), and keeps an
      overlapped `WSARecvFrom` posted on it (`sub_83143C40`; `WSA_IO_PENDING` hands the
      OVERLAPPED to an I/O object that waits on its event, `sub_831425C8` collects it
      with `WSAGetOverlappedResult`). The SDK's `WSARecvFrom` always fails, so the socket
      was closed again at once. `net.cpp` now does overlapped receives on the virtual
      sockets (pending receives complete when a datagram arrives: buffers, sender,
      OVERLAPPED status and byte count, then its event), `WSAGetOverlappedResult`,
      `WSACancelOverlappedIO`, `WSAEventSelect`, `WSASendTo`/`WSASend` (gathered,
      completed at once), `getsockname`, `getsockopt` (it probes `SO_RCVBUF` first),
      `getpeername`. The game's Winsock wrappers are at `0x82A628C8`-`0x82A62B88`.
  - Multiplayer now opens the PARTY LOBBY (Find Game, Find Custom Game, Create Custom
    Game, Difficulty, Invite Friends) with the player listed and a hosted party session
    (flags `0x823`: host, presence, peer network, friends join via presence).
  - "The Horizon server is currently unavailable, you will not be awarded XP for online
    multiplayer games": Turn 10's title services. The game enumerates title servers
    every 30 s and keeps one whose description matches the service name it wants
    (wildcards `*`/`?`, `sub_83153D48` called from `sub_83155238`); it talks "Xls Client"
    HTTP/1.1 to it (`POST /Services/o.xtsw`, `Content-Type: bin/xtsw`, `X-TitleId`,
    `X-ClassName`, `X-Method`, `X-UserId`, `X-TransactionId` headers). Our title server
    is now described as `*`, so every service resolves to the Forza Live server, whose
    `forza_live_lsp.py` logs the requests.
  - MISSING CONTENT: not reproduced in the user's flow (main menu, Multiplayer) with all
    23 DLC packages installed. The license path was checked: each DLC's license mask
    comes from `XContentCreateEx` (`sub_825166xx`, the package header written by
    `tools/install_dlc.py`: all bits) and feeds `UPDATE ContentOffers SET IsInstalled=1,
    IsPurchased=(LicenseMask & %u)=LicenseMask` (`sub_825162D0`); the only
    `XamContentGetLicenseMask` caller (`sub_82C01748`) ignores the mask, so the SDK's
    `license_mask` setting does not matter here. The messages are chosen in
    `sub_826B4200` (lobby: DLC / Disc 2 / unknown) and by a content check
    (`sub_825A56E0` returning 4) before `0x8275C238`.
- **Status and open items (end of 2026-10-10):**
  - Xbox LIVE: signed in, Gold; two players in one party, online free roam and an online
    race work (see "two players online" below). Open: matchmaking ("Find Game", "Find
    Custom Game") and Turn 10's "Xls" title services (XP), which come together.
  - MISSING CONTENT: not reproduced (see above). Next: the user's exact flow with
    `--xenia` and the place it shows; log `sub_825A56E0`'s result and the content id.
  - Free-roam physics crash (`sub_82D3DB00`): fixed, see the next entry.
  - Performance: no clean-window numbers yet (docs/perf.md has contended ones only).
  - Frame-rate unlock: done, `frame_rate = 60` (see "60 fps" at the end of the log).

- 2026-10-10 (late night): **the free-roam physics fault is fixed: Xenon's dot product
  returns QNaN on overflow.**
  - Not the SDK bug RecompiledHaloReach found the same day (ObDereferenceObject on an
    XEnumerate task's X_KENUMERATOR, which has no dispatcher header, releases whatever
    object took its stale handle, running threads included). A port of Reach's
    `object_refs.cpp` in trace mode logged no dereference of an object the runtime does not
    own in 3 loads, 2 of which faulted. Forza has the same XAPI code (XEnumerate
    `sub_830EC788`, task body `sub_830EC680`), but this flow does not run it; the port was
    not kept.
  - The bad item indices come from a GJK simplex vertex that was never written.
    `FORZA_PHYS_SEED=1` (new hooks in `src/hooks/physics_loader_check.cpp`: before each
    query, fill the w lanes of the EPA workspace's 30 vertex slots and of each new simplex's
    four vertices with sentinels naming the slot) made the fault hit 3 of 3 runs, always
    with simplex vertex 0's sentinels (`+396 CCCC00C0 +412 BBBB00C0`). Without seeding that
    slot holds whatever an earlier call left on the stack (0xBEBEBEBE is the SDK's fill of
    a new thread stack, 0x82B8D9C0 a function pointer), so it faulted only when that
    leftover made a large index.
  - Why: the GJK (`sub_82D43290`) starts a new pair with the estimate v = splat(-FLT_MAX)
    (`sub_82D12178`) and the previous squared distance (FLT_MAX, -FLT_MAX, 0, 0)
    (`sub_82D41210`). Its termination test (`sub_82D420F0`) computes |v|^2 with
    `vmsum3fp128` at `0x82D42120` and stops when FLT_EPSILON * previous >= previous - |v|^2
    in every lane. 3 * FLT_MAX^2 overflows. Xenon's VMX128 dot products return QNaN on
    overflow (Xenia emulates this, see its `use_fast_dot_product` option), which makes the
    test false. The codegen's SSE `dpps` returns +infinity, so the test passed, GJK
    returned before adding its first support point, and the closest points came from an
    empty simplex (`sub_82D41608` -> `sub_82D0F6E0`), their w lanes from vertex 0.
  - Fix: `src/hooks/gjk_dot_overflow.cpp` (`hints/gjk_dot_overflow.toml`), a hook right
    after that `vmsum3fp128` that turns an overflow of finite inputs into QNaN
    (superseded by SDK patch 0025 the same night, see below; the hook is gone).
    `FORZA_GJK_DOT_INF=1` brought the infinity back. The general fix belongs in the SDK's
    codegen (`build_vmsum3fp128`/`build_vmsum4fp128` in `sdk/src/codegen/builders/vector.cpp`
    emit a bare `simde_mm_dp_ps`); not done here, because it needs a codegen build and a
    rebuild of all generated code. Other dot products in the game may rely on the same
    overflow result.
  - Measured with a repeat script (now `tools/repro_freeroam.sh`; the user's Xenia save, START, Single Player,
    up to 2 minutes of free roam): before, 2 of 3 loads faulted
    ("about half" over the earlier runs); with seeding, 3 of 3. After: 10 of 10 loaded and
    ran free roam without a fault (each ~2 minutes in, 30 fps, the Aventador on the road
    in a screenshot); with seeding, 2 of 2.
- 2026-10-10 (late night, later): **every dot product now overflows to QNaN, in the codegen
  (SDK patch 0025, shared with RecompiledHaloReach).**
  - `build_vmsum3fp128`/`build_vmsum4fp128` keep `dpps` and branch on the exponent of its
    result; a non-finite result is redone as Xenia's `DOT_PRODUCT_3/4` (float64 products and
    sums, in Xenia's order): an overflow of finite inputs is QNaN `0x7FC00000`, infinities
    and NaNs from the inputs pass through. 2883 sites in this game. Reported upstream as
    rexglue/rexglue-sdk#491, with new cases in the SDK's PPC tests (all 1468 pass).
  - `tools/build_rexglue_sdk.sh` now also builds `rexglue` into the overlay's `bin/`, and
    the build runs codegen with it (`FORZA_REXGLUE`); a changed tool drops codegen's
    stamps, so the next build regenerates everything. The GJK hook
    (`gjk_dot_overflow.cpp`, `FORZA_GJK_DOT_INF`) is gone.
  - `tools/repro_freeroam.sh` (the user's Xenia save): 10 of 10 loads ran free roam without a
    fault (29.8-30.0 fps at 175 s), with the codegen fix alone.

- 2026-10-10 (night, later): **two players online: one party, online free roam and an
  online race, each seeing the other's car.**
  - The last two-instance test misread the menus: one scripted DOWN (0.3 s hold) moves
    the lobby cursor two rows, so DriverB had picked "Create Custom Game", not "Find Custom
    Game" (`DOWN:0.1` moves one row). Neither instance ever searched for a session.
  - "Invite Friends" in the party lobby calls `XamShowFriendsUI` (the Guide's friends
    list; the player sends invites from there), an SDK stub. `live_xmsg.cpp` now answers
    `XamShowFriendsUI` and `XamShowGameInviteUI` by inviting every other player in the
    server room to the session a friend should join (`InviteSession`). The rest was
    already there: the invitee's game auto-accepts (`live_accept_invites`), takes the
    session from `XInviteGetAcceptedInfo`, leaves its own party (`XSessionLeave`), looks
    the host up (`XNetQosLookup`, from the host's presence), joins the party session
    (`XSessionCreate` without the host flag, `XSessionJoin`) and the two talk directly
    (UDP 2130x on one machine, "direct path to player").
  - Test (two `tools/play.sh` instances, DriverA and DriverB, a private server on UDP
    21220 / HTTP 21221, fresh profiles): both open Multiplayer; A: Invite Friends ->
    both party lobbies list DriverA and DriverB, B shows "Please wait while the party host
    selects a race". A: Create Custom Game (a "Private Custom Circuit Race" with both
    players), Setup Game, Game Type -> Free Roam (sixth step right), B (back) -> "save
    now?" YES (NO drops the changes), Start Free Roam; B: Ready. Both load online free
    roam at the festival ("Co-op challenges"; host: "select a co-op challenge from the
    pause menu", B: "waiting for host"), with DriverA and DriverB in the HUD list. A sees
    B's car with its "1 DriverB" tag driving up, B sees A's. The host's game session has
    flags `0x2D` (host, stats, matchmaking, peer network); B joins it on its own (it
    hears of it through the party), both call `XSessionStart`. Peer traffic is about 65
    datagrams/s each way in free roam. Second test: a 1-lap Circuit Race (Beaumont
    Circuit) from the same lobby: both on the grid, each seeing the other's car, racing
    (A 2nd in a Fiat 500, B 1st in a Nissan Skyline). No faults in either test.
  - Matchmaking and title services: "Find Game" / "Find Custom Game" depend on Turn 10's
    hopper services (`ForzaHopperGetList`, `ForzaHopperGetSearchSettings`,
    `ForzaHopperGetSessionListFilterSettings`, `ForzaHopperReportQosData`; the lobby has
    `IDS_LobbyLspUnavailableDefaultHopper` for when they are down). The title-server
    choice was misread before: `sub_83155238` calls `Matches(mask, description)`
    (`sub_83153D48`, logged as `Matches("%s", "%s") == %d`) with the game's service mask
    as the pattern (`*`, `?`, case-insensitive; the config's `defaultMask` is `Turn10.*`)
    and our description as the text, so the description `*` never matches and the
    enumeration fails with `0x8054001F` every 30 s. A description such as
    `Turn10.Horizon` (`FORZA_LSP_SERVICES`) should make the game connect (port 80,
    `/Services.Horizon/o.xtsw` per the LspProvider config) and send its first requests.
  - Next: set the description and record the requests (`--dump-requests`); then answer
    the hopper calls in Turn 10's `bin/xtsw` format (unknown yet) so that "Find Custom
    Game" searches and players can find each other without an invite; join in progress
    from the friends list (presence) is untested.

- 2026-10-10 (late night): **Turn 10's title services: the Horizon sign-in, Find Game
  matchmaking and online XP** (format and services in `docs/title_services.md`).
  - The title server now describes itself as `Turn10.Horizon` (the game's mask is
    `Turn10.*`), and the game calls Turn 10's "Xls" services on it: HTTP POST
    `/Services.Horizon/o.xtsw`, `bin/xtsw` bodies. The format came from the game's type
    descriptors (9 words per field; member lists filled at startup, read from a running
    game's memory) and the reply parser (`sub_82AA96F0`: a 12-byte status header, then
    the results, `sub_82A95910`). Packed big-endian, u32-length-prefixed strings, byte
    arrays and arrays; the 111-byte `Logon2` request decodes exactly. No hashes or
    signatures anywhere.
  - `server/forza_xtsw.py` (codec, the message types used) and `forza_live_lsp.py`
    answer `User.Logon2`, the profile uploads, the hopper calls (`GetList`,
    `GetSearchSettings`, `GetGameOptions`, `UserRegister`, `GetRaceMultipliers`, ...);
    any other method gets success and zero bytes, which reads as empty results. The HTTP
    side keeps connections alive (the game sends `Connection: Keep-Alive`).
  - Playlists: hoppers with `maxAllowedRaces` etc. at 0 were all filtered out (the
    beginner-playlist check, `sub_828864D0`; -1 is no limit). A failed `GetGameOptions`
    disconnects; an empty one gives a lobby with "content not installed" (track 0). The
    round XML follows the game's `DefaultRacesHopperOptions.xml` (zip method 21 =
    XMemCompress LZX, decompressed with libmspack); tracks from the game database's
    `HopperRaces`.
  - Matchmaking: Find Game searches with `XSessionSearchEx` (0xB001C, property
    0x10000030 = hopper id), which went to the SDK (no results), so everyone hosted;
    `live_xmsg.cpp` now answers it like `XSessionSearch`.
  - Leaving an online race faulted both games: the game unloads and reloads
    XMediaFacade, and the SDK's unload looks the recompiled library up by the module's
    resolved path (no match), so the reload is refused ("already loaded; refusing
    duplicate load") and the game reads through a null module. `module_resident.cpp`
    keeps the facade DLLs loaded on their last `XexUnloadImage` (the SDK bug itself is
    not patched).
  - Test (two `tools/play.sh` instances, private server, `--dump-requests`): both Find
    Game -> Races; B joins A's lobby ("Clear Springs Run", intermission), both race
    (A driven along the minimap's route by a script, B stuck): A 1st, "XP REWARDS" 1,320
    XP (level 2 -> 3), B 935 XP, credits 500 -> 4,250 and 1,125; then the next round's car
    class vote. After a restart, credits came back from the save but online XP was 0:
    the game takes online XP from `Logon2` (`userData.currentXPTotal`), so the server
    keeps it (`players/<XUID>.json`, from `UpdateAll`); with that, both restarted at
    level 3 / 2 with their XP.
  - Next: the Playground playlist's game (preset 7) untested; Find Custom Game; the other
    services (storefront, auctions, scoreboards) answer empty.

- 2026-10-10 (late night): **60 fps: `frame_rate = 60`.** Details in `docs/framerate.md`.
  - The game's vblank callback (`sub_8257E888` → `sub_8247C678`) sets the main loop's
    event every second vblank; the main loop (`sub_823ED888`) waits on it and takes its
    frame time from `KeTimeStampBundle->TickCount` (whole milliseconds; a 16-17 ms frame
    counts as 1/59.94 s, the engine's 60 fps path from Forza Motorsport 4). So a 120 Hz
    guest vblank runs the game at 60 fps with its simulation still in real time; the
    checks below found nothing that counts frames or vblanks. Developer options in the binary (`unlockframerate`,
    `framesperrender`, `frontend30fps`, `lockupdatetimeto60fps`, `forcecarphysicsupdatehz`,
    ...) show the same design; retail has no command line to set them.
  - Checked: the race clock of a festival race runs at real time at 30, 60 and a 120 fps
    target (240 Hz vblank), the car accelerates the same (0-100 km/h in about 2.8 s), no
    rendering faults in free roam or races, intro videos keep their speed, 10 free-roam
    loads at 60 fps without a fault (`tools/repro_freeroam.sh`, `REX_FRAME_RATE=60`).
  - Clean window (docs/perf.md), 30 → 60: main menu 30.0 → 60.0 fps, free roam 29.9 →
    58.9 fps (p50 16.8 ms, p95 19.5), a race with 7 AI cars 30.0 → 41.6 fps (p50 24.8 ms).
    In races the render thread spins in D3D's `BlockOnFence` (`sub_823E91F0`) about 70% of
    the time, waiting for the GPU emulation, at both 120 and 240 Hz: faster GPU emulation
    is the next step for 60 in races and for more than 60.
  - `FORZA_XMP_NOSLEEP` is not needed: free roam holds 60 fps with the SDK's sleep.
  - The Xenia report (textures and physics break when unlocked): Xenia's `vsync = false`
    is a 1 kHz guest vblank, so the game runs uncapped and its frame time becomes 0-3 whole
    milliseconds; not reproduced here (the port does not run that fast).

- 2026-10-11: **a shared world: the Horizon World playlist** (phase 1 of
  `docs/online_vision.md`, which has the details, the tests and the next phases).
  - Find Game's first playlist (hopper 3) is online free roam (preset 7, festival). The
    server answers its searches with the open worlds, fullest first, so players gather in
    one world of up to 8; nobody open: the player hosts one. Searches now key on Forza's
    hopper property `0x10000030` (the client knew only Reach's `0x10000015`).
  - Find Game's lobby has no join in progress for free roam (the joiner waits for the round
    to end; Find Game's free roam runs as mode 7, a custom game's as mode 9, which offers
    JUMP IN): the world's rounds end every 10 minutes (`--world-round-minutes`).
  - Host leaving: the game migrates on its own (no `XSessionMigrateHost`, no XSession call
    at all), so guests publish a standby record of their world and the server advertises
    it once its host is gone; a returning player was placed into the remaining player's
    world. `XSessionMigrateHost` is answered anyway.
  - Cap: the round's `MaxPlayers` sizes the session (16 served -> 16 slots); the game's own
    UI and data stop at 8.

- 2026-10-11 (later): **drop-in: the Horizon World is one endless free roam that players
  JUMP IN to, and it starts with one player** (`src/hooks/world_drop_in.cpp`; details and
  tests in `docs/online_vision.md`).
  - Find Game runs as mode 7 of the online game (`*(0x83305CC4)+40`, set by `sub_828733A8`
    from the launch `sub_8287D528`; Find Custom Game is 10, a custom host 8/9). Three things
    hang on it, and the world (playlist 3, a preset 7 round, mode 7) now gets the custom
    free roam's version of each: the "custom game" check `sub_82865340` answers yes at the
    join-in-progress call sites (lobby label, JUMP IN button and its availability, the
    host taking a joiner's ready flag, peers adding the joiner's car); the launch
    `sub_8286A2E0` makes it race type 18 (online free roam flow) with HUD mode 17 instead
    of 6 (online race) / 21 (hook on `sub_825548A0`); and a host alone in its lobby gets
    the team/bucket check run (`+2152`, hook on `sub_82875FB8`), which Find Game otherwise
    runs only when a remote player's status changes, so the 5 s intermission starts.
  - Guest functions are weak symbols: a hook is a strong `extern "C" REX_FUNC(sub_X)` in
    `src/` that calls `__imp__sub_X`; `ctx.lr` holds the guest return address, so one call
    site can be told from another. No codegen or hint change, only a relink.
  - Tested with two instances: solo start, drop-in (JUMP IN, about 30 s to be driving),
    each seeing the other's car, the host leaving (the client reports the game's new host
    to the server: `MATCH_PUBLISH` op 4, from the game's host flag `+344`) and rejoining,
    and the host's END FREE ROAM (everyone back to the lobby and into the next round).
  - The free roam flow ignores a timed end condition, so the server's world rounds are
    endless by default now (`--world-round-minutes 0`).

- 2026-10-11 (night): **events from the Horizon World** (phase 2 of
  `docs/online_vision.md`: details, traces and tests there).
  - The Forza Live server starts one of the game's festival races in a world (on request,
    `GET /world/event`, or every N minutes): it tells the world's host (UDP message 18),
    whose client (`src/hooks/world_events.cpp`) asks for the next round again (the game
    asks `GetGameOptions` for round N+1 when round N starts), gets the event, and ends the
    free roam the way END FREE ROAM does (its player's status 5). The lobby waits 30 s,
    the event runs, the world comes back.
  - CPU drivers: the online lobby already has them (`MaxAIPlayers`; the host keeps
    min(MaxPlayers - humans, MaxAIPlayers), names from `AIPlayers`, cars of the bucket's
    class). To race they must be set "in game" with the host's player; kicking one throws
    the host out of its own session; a nested removal crashed the host (all fixed). With
    two consoles the CPU drivers are spread over them; both race fine, but back from the
    race one console crashes or hangs, so multi-human events run without CPU drivers
    for now (`world_event_cpu_with_peers`).
  - Hooks call guest functions on a copy of the context (`CallGuest`) and walk the player
    manager's guest `std::list` on the guest stack (`ManagerPlayers`).
  - `tools/ghidra_decomp.sh` keeps a headless Ghidra running on `ghidra_projects/ForzaHorizon`
    and decompiles functions on request (it repairs the no-return `__savegprlr` helpers
    that cut the import's functions short).
- 2026-10-11: **SDK patches 0034 and 0035 (shared with RecompiledHaloReach).** 0034 makes the
  SDK's GLSL tessellation shaders read the system constants from set 1. They read them from
  the shared memory buffer, which gave them garbage factors and index bounds. 0035 draws
  tessellated triangle strips and fans as lists (Halo: Reach's cinematic faces). Forza issues
  no such draws. Free roam was smoke-tested with both: it boots, renders and drives at 30 fps.
  Details: RecompiledHaloReach `docs/rendering.md`.

- 2026-10-12: **120 and 144 fps.** `frame_rate` takes 30 or 60-240 (details and
  measurements in `docs/framerate.md`).
  - Above 60 the game's frame time (whole milliseconds from `KeTimeStampBundle`) jittered
    5-9 ms at 144 and moved the car unevenly (16.5% frame-to-frame); `src/hooks/frame_time.cpp`
    answers the main loop's `sub_8247C168` call with `f31` from the guest tick clock (the
    nominal vblank time of an on-target frame), so frames get exactly 1/144 s and motion is
    even (2.8%). The guest vblank runs at twice the frame rate; SDK patch 0037 lets it reach
    288 Hz (the SDK capped it at 240), without which 144 needs a 144 Hz vblank and drops to
    72 fps when the GPU emulation misses a vblank.
  - Clean windows: free roam 115 fps at 120 and 112 at 144; races 76 and 70 (bound by the
    GPU emulation; the main loop still runs at the target). The race clock, the
    acceleration, an autodriven lap (114.6 / 114.6 / 115.1 s at 60 / 120 / 144) and the
    audio rate are unchanged.
  - Online: 144 against 60 races fine (each car's copy within 0.4 m of the real one, about
    20 ms ahead), the 144 player sends up to twice the datagrams (about 50 KB/s per peer).
    Rivals ghosts play back the same at 60 and 144, and one recorded at 144 is the size of
    a 30 fps one and plays back at 60 in step with the lap that made it.
- 2026-10-15: **Paint Shop / outpost garage / Autoshow crash (#42, #55, #57)**: a guest read
  of 0x1A4 on guest thread F8001188 (start `sub_82F0CF00`) as the screen opens. A libjpeg
  decode (`sub_82AC4728`, TU4) calls the CRT `setjmp` (`sub_82B3F830`); libjpeg's
  `error_exit` calls `longjmp` (`sub_82B3F300`) when the data does not decode. Both were
  plain recompiled guest code, so the longjmp returned into the host frame that called it
  with the saved registers (r31 = 0) and `read_markers` (`sub_82ADA2A0`, cinfo+420 =
  `unread_marker`) read 0x1A4. Fix: `setjmp_address` / `longjmp_address` in
  `hints_tu4/rexcrt.toml` (base: `hints/rexcrt.toml`) make the codegen emit host
  setjmp/longjmp (a codegen run, 28 files change). Needs no readback setting. Why the
  decode fails is open: the game now takes its error path instead of crashing.
- 2026-10-15: **Crash after buying the first car, then endless loading (#66, #63)**, one
  player (Windows, RX 6700 XT, v0.1.7, TU4 + DLC, `frame_rate` 240). The log: a new
  career, the "Take Her for a Spin!" achievement (156, a car bought in the Autoshow), 5 s
  later a guest read of 0x4 on the render thread (call chain `8249E118 824A1E6C 8247F9E4
  82616BFC 8261E1C0 82B3A4FC`, TU4: the render thread's command list `sub_8247F818` runs
  `sub_824A1E40`, which waits on one of 12 slots and applies its state with
  `sub_824469B0(device, slot+0x14, 0)`); every later start loads the career straight
  away and stops right after `joining session 0000000000000000`, the loader thread
  silent. Not the libjpeg longjmp (that fix is in v0.1.7). Not reproduced on Linux
  (TU4, scratch profiles, one game at a time): at 240 a new profile plays the intro and
  the opening drive; the Xenia save buys a car in the Autoshow (also at 30 and 60),
  returns to free roam with it and loads Single Player after a restart, also after a
  hard kill 5 s after the purchase. 240 is not the cause seen here. Open: the player's
  save folder (`ForzaProfile`) would show whether the load hang comes from the save.
  The fault line now names the pc as `forza.exe+0xOFFSET` on Windows, with a host stack
  (`src/debug/fault_guard.cpp`), so the next Windows crash log maps onto the executable.
  Seen on the way, every rate: the thumbnail made at a purchase is nearly black
  (`Thumbnail_46.xdc`, 23,734 bytes).

- 2026-10-16: **Endless loading after the first save (#76)**, Windows 11, RX 9070 XT, v0.1.8,
  TU4 + DLC, 240 fps, local server. The first session (new career, prologue, first race, the
  Corrado, save, Alt+F4 at the title screen) played; every later start stops right after
  `joining session 0000000000000000`. The save is not the cause: its files are complete (TU4's
  `VersionFlags` 4/2 and `NeedsAA`, sizes as ours), and a copy loads into free roam on Linux
  (TU4, 240 fps, local server) and in the TU4 Windows build under Proton/RADV with the player's
  picture settings (1440p, render scale 2, 16x AF, FXAA extreme), a warm shader cache and the
  player's DLC set (#76 and #66 have the same one: `DLCRoot1` without `zipmount.xml`, no Season
  Pass package; copied that way, it loads too). #66's last session hangs with an empty shader
  cache, so that is not it either. What the hang logs (#76, #66) share: SpeechFacade loads on a
  job worker, the game thread (`F800007C`, the `[tN]` of the `joining` line) logs the join, and
  nothing follows; normally the loading fiber unloads XMediaFacade 0.4-0.9 s later. Traced with
  gdb on Linux: after the join the game thread answers XSessionCreate, converts to a fiber,
  creates the loading fiber (`82CCB7A0`, 128 KB) and switches to it, which queues the world's
  jobs; the speech job ends (both `XamContentOpenFile("nuispeech", ...nuisp1033)` fail as in
  Xenia) and its worker goes back to its idle wait. So on the player's PC the game thread or its
  loading fiber stops before the unload, with nothing logged. Windows-only differences seen on
  the way: the I/O worker the game starts with creation flag 0x20 (`82B317A8`, base `82A746F8`:
  an IRP queue served under a spin lock) runs at THREAD_PRIORITY_ABOVE_NORMAL on Windows (the
  SDK's `set_priority(1)`; EPERM on Linux, ignored by Wine), and host fiber stacks reserve 1 MB
  on Windows (256 KB on Linux). Not reproduced; the log of the next hang will say more: closing
  the window now logs every guest thread's last call site and call chain (`LogGuestThreads`,
  `src/debug/fault_guard.cpp`).

## Debugging recipes

- Find a codegen hint's target: `tools/ppcdis.sh 0x82F76188 +0x80`.
- Decompile: `tools/ghidra_decomp.sh start`, then `tools/ghidra_decomp.sh 8287BBE0 ...`
  (C files under `/tmp/forza-decomp-$USER/out`; `stop` when done).
- Runs: `tools/run_forza.sh SECS [flags]` (logs in `$FORZA_RUN_DIR`), or
  `tools/live_session.sh DIR` + `tools/live_step.sh DIR WAIT INPUT...` (input FIFO,
  screenshot `DIR/shot.png` through `FORZA_SCREENSHOT_TRIGGER`). Headless frame dumps come
  out all zero (seen in Reach); check rendering with screenshots from a windowed run.
- Keyboard and mouse: `python3 tools/uinput_keys.py FIFO &` makes a virtual keyboard and
  mouse (uinput); `echo "tap SPACE 0.1" > FIFO` presses keys, `move 1500 0 1000` moves the
  mouse 1500 px in 1 s. Start the game with `--mnk_mode=true --mnk_mouse=true` through
  `tools/live_session.sh`, and keep its window focused: another game window opening takes
  the keys away (a game that ignores the keys is usually that). Screenshots as above.
- `FORZA_FPSLOG=1` logs the frame rate every 5 s; `FORZA_AUDIOLOG=1` the peak/RMS of the
  audio the game submits (non-zero means it makes sound), and the RMS of each of its 6
  channels (front left, front right, center, LFE, back left, back right);
  `FORZA_AUDIOLOG_MS` sets the window (default 5000).
- A game that hangs: close its window; the log ends with `FORZA: guest threads at close`, one
  line per guest thread (handle, `[tN]` on Windows, start address, the return address of its
  last call and its call chain). A thread waiting in the kernel shows the call site of the wait.
- A fault no handler takes aborts with a host stack in the log (`src/debug/fault_guard.cpp`):
  `addr2line -f -e forza-recomp/out/build/linux/forza 0xOFFSET` names the `sub_XXXXXXXX`
  frames. `FORZA_FAULT=wait` parks the thread instead, for `gdb -p`.
- Never pass two files to `xxd` (`xxd a b` writes the dump into `b`): that clobbered a
  save header once.
- Testing with the user's Xenia save without touching it: copy
  `~/xenia_canary_windows/content/E030000004B490FA/4D5309C9` into a scratch
  `<dir>/B13EBABEBABEBABE/`, symlink `<dir>/0000000000000000` to the installed DLC, run with
  `--user_data_root=<dir>` and `FORZA_XUID=E030000004B490FA`.
- `tools/repro_freeroam.sh WORKDIR N` loads free roam from a fresh copy of the Xenia save
  N times and reports faults (one game at a time). `FORZA_PHYS_SEED=1` fills never-written
  GJK/EPA vertex slots with sentinels, so reading one faults every time instead of
  sometimes.
- `tools/xrefs.py ADDR` lists branches, lis/addi pairs and data words pointing at an
  address (a quick stand-in for Ghidra's xrefs).
- Kernel imports can be overridden from `src/` by defining `extern "C" REX_FUNC(__imp__Name)`;
  `forza::SdkImport("__imp__Name")` finds the SDK's original. Guest pointers in hand-written
  code: always `forza::GuestPtr(base, addr)` (Windows maps 0xE0000000+ 4 KB further on).
- Guest address X is host `0x100000000 + X` on Linux: `gdb -p PID`, `rwatch *(int*)0x1XXXXXXXX`,
  `bt` shows the `sub_XXXXXXXX` frames.
