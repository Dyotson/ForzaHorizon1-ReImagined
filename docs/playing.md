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
- **Achievements:** unlock notifications use the native HUD with the game's trophy icon,
  achievement name and Gamerscore. Multiple unlocks queue in order; ordinary alerts and
  challenge prompts take priority, then the interrupted unlock resumes. The F7 achievement
  browser remains available in the overlay.
- **Cockpit gauges:** the game lights the instruments only when the headlights are on;
  `cockpit_gauges_lit = true` in `forza.toml` (launcher: Display, Picture, Cockpit gauges
  always lit) keeps them lit by day too, for your own car only.
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
  Multiplayer; one player picks Invite Friends in the party lobby, which opens the invite
  picker (there is no Guide): type part of a gamertag to search the players online (Up/Down
  or the D-pad, LB/RB jump to the next letter), A or Enter picks a player (up to 4), Start
  or "Send invites" sends, B or Escape cancels. Only the players picked get an invite. An
  invited player sees "X invited you to their game": hold Back + A (keyboard: Tab + Space,
  or Enter) to join, Back + B or Escape to ignore (that player's invites stay hidden for 2
  minutes), or let it go away after 20 s. The picker lists your friends online first, then
  everyone else online.
  **Friends (#92):** the server keeps your friends list; the game's friends list (who is
  online, what they are doing, joining a friend's game) shows only your friends. Press `F8`
  (`live_friends_key` in `forza.toml`) or hold the left stick press (`F` with `mnk_mode`)
  to open the friends screen over the game (Settings → Online → Controller friends shortcut
  turns the left stick hold off for players who hold it while driving, #194); B or Escape closes it. Its tabs (LB/RB or
  Left/Right): **Friends**, **Requests** (asks you, or waiting for an answer), **Add
  friend** and **Blocked**. On Add friend, type part of a gamertag (keyboard) or press Y
  for the on-screen keyboard (D-pad moves, A types, X deletes, Y space, B done); it lists
  the matching players online and those the server knows. A or Enter on a player offers
  what you can do: send a friend request, accept, decline, take a request back, remove,
  block, unblock. A request or an accepted one shows a short notice. A blocked player can't
  invite you or send you requests (they are dropped without telling them). Your friends
  are tied to `friends_key` in `live_identity.txt`: keep that file (it also holds your
  XUID) to keep your friends on another computer. With a server older than this, everyone
  online counts as a friend, as before.
  **Text chat (#92):** `F9` (`live_chat_key`) opens the friends screen on its **Chat** tab
  (with a controller: hold the left stick press, then LB to Chat; a friend's "Send a
  message" goes there too). Up/Down picks where the message goes: **Session** (the players
  in your party or game session), **Horizon World** (the players in your world), or a
  friend online (a direct message; only friends). Type and press Enter; with a controller,
  A or Y opens the on-screen keyboard and Start sends. A message that arrives while the tab
  isn't open pops up for 6 s. "Mute in chat" in a player's menu hides their messages (also
  players who are not your friends: find them on Add friend); blocking hides them both ways.
  At most 200 characters a message and 5 messages in 10 s. Messages are not stored: the
  server passes them on to the players online and keeps only the last 200 in memory for its
  admin. A player in free roam restarts the game to join,
  as on the console, which takes a few seconds. `live_accept_invites = false` ignores all
  invites. Games up to v0.1.12 invited everyone online and joined every invite at once
  (#90). The party leader then picks Create Custom Game, sets the game
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
- **Driving locked after a save loads, only START works:** that is the 1000 CLUB tutorial
  (TU4, signed in to Forza Live; #237). Once a career is past the point where the 1000 CLUB
  opens, the game plays its intro and then gives the car no throttle, steering, camera or
  map until the tutorial is done, at every load until then. The hints at the top left and
  bottom say what to do: START, **1000 CLUB**, select the challenge card, pick a car and
  **Begin challenges in this car**, then D-pad up for **Current car challenges** and B.
  The launcher's **Edit profile…** can also mark it finished.
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
  The launcher's **Photo gallery** browses local saved pictures and exports PNG/JPEG
  copies. Press Refresh after saving a new picture ([gallery details](launcher.md#photo-gallery)).
- **Hitches:** a frame that takes over 200 ms logs `FORZA_STALL` lines in `forza.log` with
  what the game was doing; attach them to an issue ([details](perf.md#long-frames-forza_stall)).
- **No sound:** when the sound output cannot be opened (another program holds it in exclusive
  mode, Windows refuses its format, no audio server on Linux), the game runs on without sound and
  `forza.log` says `No audio output could be opened (<reason>)`. It asks for the output again every
  5 s (unless SDL found no audio driver at all), so the sound comes back once the device is free.
  Builds before SDK patch 0103 stopped there (#244). On Windows, try another default output, or
  untick "Allow applications to take exclusive control" in the device's Properties > Advanced.

## Recommended picture settings

- Upscaling filter **AMD FSR 1** (the launcher's first-run default) at render scale 1: a sharp
  image at 1080p, 1440p or 4K for about the cost of bilinear. `forza.toml`: `present_effect = "fsr"`.
- Render scale 2 adds real detail for about 30 % fewer fps (42 fps at a 60 fps target on an
  RX 9060 XT); fine at 30 fps. Above 2 is experimental and halves the frame rate (#47).
- **AMD FSR 3 (temporal)** is a heavier, temporal upscaler; on integrated graphics and the Deck, FSR 1 is the better choice. `cas` only sharpens. See `docs/rendering.md`.

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

## Racing wheels (issue #4)

Any wheel SDL sees as a joystick (Logitech, Thrustmaster, Fanatec, Moza, ...) is presented to the game as the
**Xbox 360 Wireless Racing Wheel**: the game asks the controller for its capabilities, and a wheel answers
SubType 2 with the force feedback and wireless flags, which is what FH1 checks for. Steering comes in on the
left stick X over its full range, throttle and brake on the right and left trigger, the buttons on A B X Y,
the bumpers, Back and Start, and a hat as the d-pad. The clutch pedal and an H-pattern shifter go in the way
the game reads them from the 360 wheel (below). Gamepads and keyboard and mouse work as before.

The game then treats it as a wheel: My Profile, Options, Controls shows the *Wheel layout* (the paddles shift,
the face buttons are clutch, handbrake, Activate and Rewind), its Advanced wheel options (steering, pedal and
clutch deadzones, force feedback scale) apply, the car uses the game's full-range wheel steering and the game
sends its own force feedback (below). Until #219 a bug in our capabilities hook hid the wheel from the game,
which took every wheel for a gamepad (found by a G923 owner in #4).

Tested here with a virtual wheel (Linux uinput) and by a Logitech G29 owner and a G923 for Xbox owner on Windows
(issue #4: steering, pedals, buttons and force feedback work; the G923 also the clutch and H-shifter). Other wheels
are untested: please report the model and what works.

**Known wheels** (Logitech G25, G27, G29, G920, G923, Driving Force GT, by USB vendor and product id) have a
built-in profile: fixed axes, pedals that rest at the maximum and read the minimum when pressed, and for the
G29, G920 and both G923 versions their own button and shifter maps (below). The axes are never guessed from where they sit at start-up for these wheels.
The profile numbers are SDL's: on Windows steering 0, throttle 1, brake 2, clutch 3; on Linux steering 0,
clutch 1, throttle 2, brake 3. If a pedal then works backwards on your system, switch `wheel_invert_throttle`
or `wheel_invert_brake`; `wheel_axes` and `wheel_buttons` still override the profile. Other wheels are detected
as described below. A pedal is also ignored until it has been seen at rest once, so a pedal that reads 0 for a
moment when the game opens it (or is held down at start) no longer presses itself.

Setup (launcher: Controls, Racing wheel; the keys are the `forza.toml` names):

| Setting | Default | What it does |
| --- | --- | --- |
| `wheel_mode` | `auto` | `auto`: SDL says it is a wheel, or its name looks like one (G29, T300, Fanatec, Moza, ...). `on`: every controller without a gamepad mapping is a wheel. `off`: wheel support off |
| `wheel_rotation`, `wheel_lock` | 900, 900 | The wheel's rotation as set in its driver, and the travel (lock to lock) that gives the game's full steering. 900/540 makes the steering about 1.7 times quicker |
| `wheel_deadzone`, `wheel_invert_steering` | 0, off | Steering deadzone (0 to 0.5); swap left and right |
| `wheel_steer_offset` | 0.05 | Only with `wheel_900_steering` off: compensates an inner steering deadzone (below) |
| `wheel_pedals` | `auto` | Separate pedals; `combined` is one axis with throttle one way and brake the other |
| `wheel_invert_throttle`, `wheel_invert_brake` | off | Pedals found backwards |
| `wheel_throttle_curve`, `wheel_brake_curve` | 1.0 | Gamma: 1 linear, above 1 softer at the start |
| `wheel_axes` | detected | e.g. `steer=0,throttle=2,brake=5,clutch=1` |
| `wheel_buttons` | see below | e.g. `a=0,b=1,x=2,y=3,lb=5,rb=4,back=8,start=9,ls=11,rs=10,up=12,down=13,left=14,right=15` (SDL button numbers; -1 unmaps). A button the shifter uses presses none of these |
| `wheel_ffb`, `wheel_ffb_strength` | on, 60 | Force feedback switch and strength (0 to 100 percent) |
| `wheel_ffb_spring_strength` | 100 | The centring spring and the damper, in percent of `wheel_ffb_strength` (launcher: Centring strength) |
| `wheel_rumble_engine_strength`, `wheel_rumble_road_strength` | 100, 100 | The rumble from the game's engine motor (its right motor) and from its road motor (left: road surface, kerbs, impacts), in percent of `wheel_ffb_strength` (launcher: Engine rumble, Road rumble) |
| `wheel_ffb_reverse` | off | Reverse the game's steering force, for a wheel that pulls away from the centre in corners |
| `wheel_clutch`, `wheel_invert_clutch` | on, off | The clutch pedal as the game's analog clutch; a clutch found backwards |
| `wheel_shifter` | `off` | `h`: an H-pattern shifter selects the gear; `sequential`: its up and down buttons shift like the paddles |
| `wheel_shifter_buttons` | by model | e.g. `1=12,2=13,3=14,4=15,5=16,6=17,r=18` (gears 1 to 7 and reverse) or `up=..,down=..`; SDL button numbers, -1 unmaps |
| `wheel_shifter_clutch`, `wheel_clutch_threshold` | off, 0.5 | H-pattern: a gear engages only with the clutch pressed this far (0.1 to 1) |

**Steering around the centre.** A G29 owner reported that the first degrees of every turn did nothing: the game
took the wheel for a gamepad then (#219) and applied a gamepad's inner deadzone, and a 900 degree wheel turned 50
degrees is only 5.5 percent of its range. `wheel_steer_offset` compensated: any movement beyond a hair (about 1
degree) already gives that share of the full steering (0.05 = 5 percent). Now that the game knows the wheel, its
full-range steering (`wheel_900_steering`, on by default) follows the wheel's angle from the centre, and the
offset is not applied there: its step at the centre made the car twitch left and right when driving straight
(#219). It still applies in the 270 degree mode (`wheel_900_steering = false`), where the game filters the
steering. The game's own wheel deadzones are in its Advanced wheel options.

**Clutch pedal and H-shifter (issue #125).** The game has both for the 360 wheel (third-party 360 wheels had
them; on the Xbox One a G920's clutch and shifter work in this game). Its wheel layouts bind the right stick Y
to the clutch, and when the wheel's capabilities say it has an H-pattern shifter, the car takes the gear from
the right stick X (-1 reverse, 0 neutral, 1 to 8). So the driver sends the clutch pedal (0 released, full
travel pressed) on the right stick Y and, with `wheel_shifter = h`, the gear of the held shifter button on the
right stick X; no gear button held is neutral. The game's own gearbox does the shift: one shift with its normal
shift time, also from 2nd straight to 5th. The game's *Wheel layout* screen (My Profile, Options, Controls)
shows the rest of the wheel's controls; in Wheel layout 1 the bumpers shift up and down and A is the clutch
button.

- **Clutch:** choose *Manual with clutch* (pause menu: My Profile, Options, Difficulty, Shifting), then the
  pedal works like the clutch on a real car (pressed, the engine revs freely and the car does not pull) and the
  game's wheel clutch deadzones apply. The wheel's clutch axis is the one
  from the profile (Logitech) or `wheel_axes` (`clutch=N`), else the third pedal found at start. Off with
  `wheel_clutch = false`.
- **H-pattern shifter:** set `wheel_shifter = h` (launcher: Controls, Shifter and clutch, Shifter: H-pattern)
  and *Manual* or *Manual with clutch* shifting in the game; with *Automatic* the game ignores the shifter.
  Even with *Manual with clutch* the game takes the lever's gear without the clutch (see realistic shifting).
  The Logitech Driving Force Shifter on a G29, G920 or G923 is built in, by the wheel's USB product id: gears 1
  to 6 are buttons 12 to 17 on all of them, reverse (pushed down) is 18 on a G29 or a G923 for PlayStation
  (046d:c24f, c266) and 11 on a G920 or a G923 for Xbox (c262, c26e): those wheels number reverse before the gears
  (Logitech's and PCGamingWiki's button tables, and a G923 for Xbox owner, #256). Other shifters need
  `wheel_shifter_buttons`, e.g.
  `1=8,2=9,3=10,4=11,5=12,6=13,r=14`; up to 7 gears and reverse (the log prints the map: `Wheel: H-pattern
  shifter, buttons r=...`). A shifter that is its own USB device (Thrustmaster TH8A, Fanatec USB shifters) is
  not read yet: only buttons of the wheel itself.
- **The shifter's buttons press nothing else.** Every button the shifter uses (its gears and reverse with `h`, up
  and down with `sequential`) is taken out of the wheel's pad buttons, whatever the defaults or `wheel_buttons`
  say: on a G923 for Xbox reverse is button 11, which the generic map had as the left stick press, so engaging
  reverse also opened the friends screen (#256).
- **Realistic shifting:** `wheel_shifter_clutch` makes a new gear engage only while the clutch is pressed at
  least `wheel_clutch_threshold` (default half way); moved into a gear without the clutch, the car stays in
  neutral until you press it. Off by default (the lever's gear engages at once).
- **Sequential:** `wheel_shifter = sequential` with `wheel_shifter_buttons = up=N,down=M` adds a sequential
  shifter next to the paddles (up and down act as the right and left bumper, the wheel layouts' shift up and
  shift down). At a standstill the game's downshift drops to 1st, as with the bumpers.

Tested with a virtual G29 with Driving Force Shifter and clutch pedal (Linux uinput): gears 1 to 6, reverse and
neutral, 2nd straight to 5th at speed, the clutch holding the car while the engine revs in *Manual with clutch*,
realistic shifting and the sequential buttons; the shifter is ignored with *Automatic*. Not tested with a real
shifter or clutch pedal yet: please report the model and what works.

The shifter type is read when the game first sees the wheel, so restart the game after changing it. The clutch
and gear are sent only once the game has read the wheel's capabilities (the log says `Wheel: the game asked for
capabilities`); a game that took the wheel for a gamepad would turn them into right stick movement. They share
the right stick with any other controller connected at the same time (the stick pushed furthest wins), so a
gamepad whose right stick drifts can disturb the gear: unplug it while driving with the wheel. Keep
`right_stick_deadzone_percentage` at 0 for the same reason.

Axis detection (wheels without a profile) reads where each axis rests when the wheel is plugged in: the one near
the middle is the steering, those resting at an end are the pedals (throttle, brake, clutch in axis order).
**Keep the pedals released while plugging in or starting the game**; a pedal that works backwards is
fixed with the invert switch, a wrong axis with `wheel_axes`. The log line `Wheel: "..." axes(..) buttons(..)`
and the `axis N rests at` lines show what was found. Default buttons: 0-3 A B X Y, 4 right bumper, 5 left bumper,
8 Back, 9 Start, 10 and 11 stick presses; the G29 and G923 for PlayStation map is
`a=0,b=2,x=1,y=3,rb=4,lb=5,back=8,start=9,ls=11,rs=10` (cross, circle, square, triangle, right paddle, left
paddle, Share, Options, L3, R3; R2 and L2 are free). The G920 and G923 for Xbox map is
`a=0,b=1,x=2,y=3,rb=4,lb=5,start=6,back=7,rs=8,ls=9` (Menu, View, RSB, LSB; the Xbox button, 10, and the shifter's
11 to 17 are free): their buttons follow PCGamingWiki's G920 and Xbox G923 tables, where the generic map had put
the stick presses on the Xbox button and the shifter's reverse (#256, not yet confirmed on the wheel). With the
game's Wheel layout 1 the paddles shift up and down, cross is the clutch, circle the handbrake, square Activate,
triangle Rewind and Share the camera (the paddles were on B and X while the game took the wheel for a gamepad,
whose layout shifts with those). Wheel buttons differ between models, so check the numbers (for example
with `jstest-gtk`) and override as needed. Wheels that SDL also maps as a gamepad are taken by the wheel driver
instead; Xbox (Microsoft) wheels keep using the gamepad path.

**Force feedback (#219).** The game drives the 360 wheel's force feedback itself, through the kernel's
`XInputdFF*` calls, which SDK patch 0101 plays on the wheel with SDL haptics. It sets up five effects and keeps
them running: a **constant force** that it updates every frame with the car's steering force (in a corner it pulls
the wheel back towards the centre; scraping a wall or crashing makes it kick), a **centring spring** that is off
at a standstill, grows with speed and fades while the rear wheels slide, a light **damper**, and an inertia and a
friction effect set to zero. Its tuning is `media/ControllerFFB.ini`; the game's Advanced wheel options scale it
(Force feedback scale) and the rumble (Vibration scale). The rumble (engine, road surface, impacts) comes as a 25 Hz
vibration. `wheel_ffb_strength` scales everything (default 60 percent), `wheel_ffb = false` switches it off.
Three more strengths (#256), in percent of it and 100 by default (as before), each set one part:
`wheel_ffb_spring_strength` the centring spring and the damper (the spring grows with speed, so at speed small
turns can feel heavy; lowering it softens the centring without weakening the steering force and the road feel),
`wheel_rumble_engine_strength` the game's right motor, which carries the engine and climbs towards the limiter, and
`wheel_rumble_road_strength` its left motor (road surface, kerbs, impacts). Both motors play as one vibration, the
stronger of the two after their strengths, so in a car that revs quickly a lower engine strength lets the road
through. The
wheel's own autocentre is off while the game runs; on a wheel without a spring effect (the Linux kernel driver of
the G25, G27 and G29 has only constant force and autocentre) the game's spring sets the autocentre strength
instead. On Linux the wheel needs read and write access to its `/dev/input/event*` node (a udev rule for most
wheels).

**Not driving: the forces fade out (#256).** The game keeps its wheel effects going when the car is not under the
player's control: in the pause menu it sets the centring spring to full and leaves it and the damper on, a
rewind plays the steering force and rumble of the car going backwards, and replays keep the last spring. So the
wheel pulled and buzzed there (a G923 owner, #4). The driver now fades all the game's forces and its rumble to
nothing in 0.25 s while the player is not driving and back in over 0.5 s when they are. The game tells it
(`ForzaWheelPlayerDriving`, an export of the runtime) from the car's physics: its substeps stop in the pause menu,
rewinds, replays, menus and loading screens, and driving counts again once they have run for half a second (which
leaves out most of the short bursts when a replay starts; the one when the rewind menu's *View replay* starts is
1.3 s long). The game's rumble cannot tell with a wheel: it sends a wheel's rumble only through the force feedback
calls. With a virtual G923: in the pause menu and while rewinding the spring, damper, constant force and rumble the
wheel receives go to 0 within half a second, and come back half a second after *Resume* or *Resume from here*.
`FORZA_WHEEL_DRIVING_GATE=0` switches the fade off.

Tested with a virtual wheel (uinput) that logs what it receives, and by a G29 and a G923 owner. Wheel drivers disagree about
which way a positive force turns the wheel: if yours pulls further into a corner instead of back to the centre,
turn on `wheel_ffb_reverse` (launcher: Reverse force direction), and please report the model.

A game that takes the wheel for a gamepad only sets the vibration motors; the driver then makes an approximate
centring spring and vibration from them.
