# Playing: details

Moved out of the README; the short version is there.

## Running the game

```sh
tools/play.sh                 # play with your own profile (~/.local/share/forza)
tools/play.sh --xenia         # continue your Xenia Canary save (a copy; see below)
tools/play.sh --offline       # without Xbox LIVE
tools/play.sh --base          # the base game instead of Title Update 4
tools/run_forza.sh 120        # a two-minute test run; logs in ${XDG_RUNTIME_DIR:-/tmp}/forza_run
```

- **Profile and controllers:** the game runs signed in to one local profile on player 1,
  and every connected controller drives player 1, so START on any pad gets you past the
  title screen (with Steam Input or Sunshine virtual pads around, your pad is often not
  the first one, and the game used to ask that pad to sign in).
  `FORZA_PAD_PER_USER=1` gives each pad its own player again.
- **Keyboard and mouse:** `mnk_mode = true` in `forza.toml` (or `--mnk_mode=true`, or the
  launcher's Controls page); bindings and test results [below](#keyboard-and-mouse).
- **Xbox LIVE:** the game signs in to an emulated Xbox LIVE, as a Gold member, through a
  Forza Live server (`server/forza_live_server.py`, Python 3 standard library only).
  `play.sh` starts one on this machine for the session (UDP 21200, HTTP 21201, data in
  `~/.local/share/forza/live_server/`), or uses the one already running there. To play
  with others, run the server where everyone can reach it
  (`python3 server/forza_live_server.py --http-port 21201`) and start the game with
  `FORZA_SERVER=host[:port]` (or `live_server = "host:port"` in `forza.toml` next to the
  binary). Your online identity (XUID, gamertag: your login name unless
  `FORZA_GAMERTAG` is set) is kept in `~/.local/share/forza/4D5309C9/live_identity.txt`.
  Everyone in the server's room is your friend. Title Update 4 and base-game players
  can't play together: a TU4 game uses the room's "#TU4" version, so each meets only
  players of its own version.
  **The shared world:** Multiplayer, Find Game, **Horizon World** (the first playlist).
  The server puts you into the fullest open world (8 players each) or you open a new one
  and start driving at once, alone, and everyone who picks it after you is placed with
  you: their lobby says FREE ROAM IN PROGRESS, and **JUMP IN** drives them into the
  running world. The world is an endless online free roam (co-op challenges from the
  host's pause menu); when its host leaves, the others keep driving and the next players
  are placed with them. The host's END FREE ROAM takes everyone back to the lobby, and the
  world starts again after 5 s. `world_drop_in = false` in `forza.toml` turns this off
  (the world is then Find Game's race-like free roam, which a late player only enters
  after the round; `--world-round-minutes` on the server ends those rounds on a timer).
  Details: [`docs/online_vision.md`](docs/online_vision.md).
  To play together in a party: everyone opens
  Multiplayer; one player picks Invite Friends in the party lobby (there is no Guide, so
  this invites everyone in the room) and the others join that party on their own
  (`live_accept_invites`). The party leader then picks Create Custom Game, sets the game
  type under Setup Game (Free Roam, races, ...), saves the setup (YES) and starts once
  the others are ready. Or, without a party: Find Game, then the Races playlist; the
  first player hosts the playlist's lobby and the next ones join it, and the race starts
  after the intermission once two players are in. The server also stands in for Turn 10's
  title services ("the Horizon server", `docs/title_services.md`): the playlists, the
  rounds' tracks, and online XP, which it keeps per player (`players/` in its data
  directory) and gives back at every sign-in; credits stay in your save. It also keeps
  every player's best lap on each event with its ghost (`scoreboards.json`, `ghosts/`):
  Race Central's LEADERBOARDS show them, and X RIVALS on an event's ticket races you
  against another player's ghost for a bounty.
- **Frame rate:** `frame_rate = 60` in `forza.toml` (or `tools/play.sh --frame_rate=60`)
  runs the game at 60 fps instead of the console's 30; 120, 144 or any rate up to 240 work
  too. The game times its physics and clocks in real time, so they keep their speed, and
  players at different frame rates race together. Free roam runs 60 fps at 60, about 115
  at 120 and 112 at 144; a race with 7 AI cars 60, 76 and 70, bound by the GPU emulation
  ([`docs/framerate.md`](docs/framerate.md)). 144 needs the SDK overlay with patch 0037
  (`tools/build_rexglue_sdk.sh`); without it 144 drops to about 72 in free roam.
- **Title Update 4 (default):** `play.sh` runs `forza-recomp/out/build/linux-tu4` when it
  exists (how to build it: README, Building; `FORZA_BUILD=linux-tu4`) and mounts the
  update's files (`extracted/tu4/`, or `FORZA_UPDATE`) as `update:` with
  `--update_data_root`. Horizon Rally and the 1000 Club are in this build only, and the
  Xenia save of a TU4 player needs it. TU4 converts a profile save when it loads it
  (`ForzaProfile/VersionFlags` 0 to 4, flags 2, a `NeedsAA` file); the base build then
  refuses the profile ("MISSING TITLE UPDATE: Your profile save game requires the latest
  title update"). `--base` (or `FORZA_BUILD=linux`) runs the base build: only for profiles
  that never ran on TU4, so keep a copy of the profile folder before the first TU4 run.
  Without a TU4 build `play.sh` uses the base one. Details: [`docs/dlc.md`](dlc.md).
- **DLC:** install your DLC packages (the `4D5309C9/00000002/` folder of your content)
  once with `python3 tools/install_dlc.py /path/to/4D5309C9/00000002` (all 23 packages; a
  Xenia `content` folder with the DLC already extracted works too, or its `00000002` folder,
  and so does the launcher's DLC step;
  the Rally Expansion's and the 1000 Club's modes need the TU4 build, their cars work on both).
- **Saves** live in `~/.local/share/forza/B13EBABEBABEBABE/4D5309C9/`.
- **Your Xenia save:** `tools/play.sh --xenia` copies your Xenia Canary profile's Forza
  Horizon save (`~/xenia_canary_windows/content/<XUID>/4D5309C9`, or
  `$FORZA_XENIA_CONTENT/<XUID>/4D5309C9`) into `~/.local/share/forza/xenia-<XUID>/` and
  plays it signed in with that XUID: Forza ties its save to the profile's XUID, and
  refuses (freezes on the title screen) a save made under another one. The copy is made
  once, so progress made here stays in it; `--xenia-refresh` copies the Xenia save again.
  The Xenia original is never modified. With several Xenia profiles, name one:
  `tools/play.sh --xenia E030000004B490FA`. (Equivalent by hand: copy the save under the
  runtime's profile folder and start with `--profile_xuid=<XUID>`.)
- **Car pictures:** the game renders each car's picture (Car Select, the garage tiles) once
  and saves it in the profile (`ForzaProfile/Thumbnails/`). Builds before resolve readback
  was turned on (`readback_resolve = "some"`, now the default) saved noise there, and so did
  Xenia without that setting. Those pictures stay broken until the car changes: an upgrade
  or a new paint job in the garage renders it again. Deleting the files only makes them
  black. `readback_resolve = "none"` in `forza.toml` turns readback off (no measurable gain over
  `some`, broken pictures again).
- **Photos:** photo mode saves the picture in the profile (`photo/<name>/`); SAVE asks for
  a name in a small window over the game (Enter or OK). Builds before SDK patch 0048 saved
  a garbled picture (#54); photos saved by them stay garbled, take them again.

## Recommended picture settings

- Upscaling filter **AMD FSR 1** (the launcher's first-run default) at render scale 1: a sharp
  image at 1080p, 1440p or 4K for about the cost of bilinear. `forza.toml`: `present_effect = "fsr"`.
- Render scale 2 adds real detail for about 30 % fewer fps (42 fps at a 60 fps target on an
  RX 9060 XT); fine at 30 fps. Above 2 is experimental and halves the frame rate (#47).
- `fsr2` / `fsr3` do not exist (no temporal data); `cas` only sharpens. See `docs/rendering.md`.

## Steam Deck

The launcher detects the Deck and applies its preset: Vulkan, render scale 1, AMD FSR 1, `frame_rate = 30`,
background shader compilation and the shader cache on. Keep these: render scale 2 and `frame_rate = 60` only
make it slower. Play a session or two on a warm shader cache before judging the speed. Reported speed on a
Steam Deck OLED (#45): 17 to 25 fps. We have no Deck; with a PC restricted to 4 cores that each give the game a
fifth of a core, a race runs 22 to 24 fps (`docs/perf.md`, "Steam Deck approximation"), so the limit is the
game's main thread and the graphics command thread, and none of the settings (MSAA, readback, submission thread,
thread priorities) changes it. Cutscene audio that does not match the picture comes with the low frame rate;
it has not been reproduced here.

## Keyboard and mouse

`mnk_mode = true` makes the SDK present the keyboard (and mouse) as a controller on player 1.
It was played end to end this way: menus, free roam, a Festival race from the sign-up to the
countdown and the race itself, the pause menu, the map and photo mode. Keys work while the
game window has focus.

| Controller | Default keys | In the game |
| --- | --- | --- |
| Left stick | `W` `A` `S` `D` | steering (`A` `D`), menu and map navigation |
| Right stick | arrow keys, or the mouse | free look, photo mode camera, map zoom |
| Right trigger | `E` or `O` | accelerate |
| Left trigger | `Q` or `I` | brake, reverse |
| A | `Space` or `;` | select, handbrake, take photo |
| B | `Backspace` or `'` | back, resume; shift up with manual gears |
| X | `L` | sign up for an event, focus in photo mode; shift down with manual gears |
| Y | `P` | rewind (Difficulty: Rewind must be on), effects in photo mode |
| Left / right bumper | `1` / `3` | (unused) / change camera |
| Left / right stick press | `F` / `K` | (unused) / look straight back |
| Back | `Z` or `Tab` | map; with manual gears, enter an event |
| Start | `X` or `Return` | pause menu, past the title screen |
| D-pad | `Shift` + arrow keys | radio |

Triggers and keys are all or nothing, as on a pad pressed to the stop; there is no analog
throttle. The left stick ramps instead (`mnk_stick_ramp`, default 0.2 s): a key moves it to
60 % at once and to full lock over 0.2 s, and releases in half that, so a tap steers
gently and a held key still reaches full lock. Without the ramp (`mnk_stick_ramp = 0`) a
key is full lock at once (a second of it took the starter car from 54 to 27 km/h).

**The mouse** (`mnk_mouse = true`) moves the right stick by its speed: 1000 pixels a second
is half the stick at `mnk_sensitivity = 1`, and the camera returns to centre when the mouse
stops, as with a stick (raise the sensitivity for a slow mouse). While the game has focus
the pointer is captured and hidden (Alt+Tab or the compositor's own shortcut leaves). The
game's menus have no pointer, so the buttons do nothing there, and the wheel is unused.

**Rebinding:** `keybind_<name>` in `forza.toml` or on the launcher's Controls page, e.g.
`keybind_right_trigger = "W,E"`. Each takes comma-separated alternatives. Names: letters,
digits, `F1`-`F24`, `Space`, `Return`, `Tab`, `Backspace`, `Escape`, `Up` `Down` `Left`
`Right`, `Semicolon`, `Quote`, `Comma`, `Period`, `Slash`, `Backslash`, `LBracket`,
`RBracket`, `Minus`, `Plus`, `Backtick`, `Numpad0`-`Numpad9`, `Home` `End` `PageUp`
`PageDown` `Insert` `Delete`, `LMB` `RMB` `MMB` (mouse buttons), and `Shift`, `Control`,
`Alt`, alone or in a combination (`Shift+Up`). A combination only fires with exactly those
modifiers down, which is why the D-pad (`Shift+Up`) and the right stick (`Up`) can share
the arrow keys. Names: `keybind_a b x y left_trigger right_trigger left_shoulder
right_shoulder lstick_up lstick_down lstick_left lstick_right lstick_press rstick_up
rstick_down rstick_left rstick_right rstick_press dpad_up dpad_down dpad_left dpad_right
back start guide`.

Results by activity:

| Activity | Result |
| --- | --- |
| Menus (title, main, pause, options, difficulty, event cards) | works; `W` `S` `A` `D` navigate (`A` `D` change a setting), `Space` selects, `Backspace` goes back; a tap of 70 ms or more registers |
| Free roam: steer, accelerate, brake, reverse, handbrake | works; steering ramps (above); throttle and brake are all or nothing |
| Gears (Difficulty: Shifting manual) | works: `Backspace` up, `L` down |
| Camera change, look back, free look (arrows or mouse), rewind | works; rewind needs Difficulty: Rewind on (it is off by default) |
| Race: sign-up (`L`, or `Z` with manual gears), enter event, start, drive, rewind, pause menu | works |
| Map | works: `WASD` moves the cursor, the arrows or the mouse zoom, `Backspace` leaves |
| Photo mode | works: `WASD` moves the camera, arrows or mouse turn it, `Space` takes the photo, `Backspace` exits |
| Awkward | no analog throttle, brake or steering; no pointer in menus; with manual gears `Z` enters events, so open the map from the pause menu |
| Not tried | the radio keys, the confirmation of "Retire from event" by keyboard (another window took focus during that test), online play |

SDK patch 0040 (`docs/sdk_patches.md`) adds the stick ramp, the mouse speed model and
modifier keys as bindings; without it `mnk_stick_ramp` does not exist and the mouse is
about five times weaker.
