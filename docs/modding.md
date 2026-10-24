# Making mods

A mod is a folder of files the game reads in place of its own. The game's files are never
changed: when the game starts, each mod's files are laid over the game's archives (#149), so
mods survive a rebuild, a launcher update and a reinstall of the game files.

Mods made from the title update's files are for the Title Update 4 build (the launcher builds it
when you give it the title update in **Setup**); the base game loads mods too, see
[How it works](#how-it-works).

## Where mods go

In the **mods** folder of the launcher's data folder; the **Mods** page of the launcher has an
**Open mods folder** button.

| System | Folder |
| --- | --- |
| Windows | `%LOCALAPPDATA%\ForzaReImagined\mods` |
| Linux | `~/.local/share/forza/mods` |
| Flatpak | `~/.var/app/<app id>/data/forza/mods` |

(If you chose another **Game data folder** in **Settings > Advanced**, `mods` is in that folder.)

Each mod is one folder in it:

```
mods/
  BetterTitle/
    mod.toml
    media/
      stringtables/en/main.str
      gamemodes/autoshows.xml
      db/gamedb.slt
```

- `media/` holds the files, at the game's own paths (below). Folders and names are not
  case-sensitive; use plain ASCII names.
- `mod.toml` (optional) names the mod for the launcher and the log:

  ```toml
  name = "Better title screen"
  version = "1.0"
  author = "You"
  description = "One line about what it changes."
  ```

The **Mods** page lists the mods, with a tick to turn each one on or off and **Move up** /
**Move down** for the load order: mods load from top to bottom, and when two mods have the same
file, the lower one wins. Changes apply the next time you press **Play**. Without the launcher
(`play.sh`, a source build), every folder in `<user data>/mods` loads, by name; the game's
settings `forza_mods_dir` and `forza_mods` (folder names in load order, separated by `|`) choose
others.

## Finding a file's path

The game sees all of its archives as one tree under `game:\media\`, and a mod file's path is its
place in that tree:

- **An archive of the disc** (`media\*.zip`, `media\cars\*.zip`, `media\stringtables\*.zip`, ...;
  `media\zipmanifest.xml` lists them all): the archive's path without `.zip` is a folder, and its
  members are inside it. `media\gamemodes.zip` member `Colorado\speed_camera.xml` is
  `media/gamemodes/colorado/speed_camera.xml`; `media\stringtables\EN.zip` member `Data_Car.str`
  is `media/stringtables/en/data_car.str`; `media\renderscenarios.zip` member `Global.xml` is
  `media/renderscenarios/global.xml`.
- **The title update's `media.zip`**: its members are already at these paths (`db/gamedb.slt`
  is `media/db/gamedb.slt`). Where the title update has a file, it is the one the game uses, so
  start a mod from the title update's copy, not the disc's.
- **A DLC package's archives** (`Media\DLCZips\` in the package): `zipmount.xml` there gives each
  archive's mount point. `StringTables_pri_1650.zip` is mounted at `game:\Media\StringTables\`,
  so its member `EN/Data_Car.str` is `media/stringtables/en/data_car.str`; the members of
  `<offer>_pri_65.zip` start with `media/` already.
- **A loose file** of the disc (`media\db\gamedb.slt`, `media\FilesToCache.xml`, ...): the same
  path. A mod file with the path of a loose file replaces it too.

Most members are compressed with the Xbox's LZX (zip method 21), which common zip tools cannot
read. `tools/unzip21.py ARCHIVE MEMBER OUTDIR` extracts one (it uses `tools/unxmem`, built from
`tools/unxmem.c`). `tools/make_example_mods.py` makes the example mods below from your own title
update and shows how.

Change files with a tool that keeps their format; a mod file is used as it is (the mod loader
stores it uncompressed in the archive, the game reads stored members as well as compressed ones).
For `gamedb.slt` (SQLite) any SQLite editor works; leave the journal mode alone (no WAL: the
game's SQLite predates it).

## What the log says

`forza.log` (in the data folder) lists the mods at start, each file and what it replaces, and the
first time the game reads each one:

```
Mods: .../mods in load order (a later mod wins): ExampleTitleText, ExampleGearbox
Mods: Example: title text 1.0 by make_example_mods.py: 1 file(s)
Mods: media/db/gamedb.slt from ExampleGearbox: in place of the title update's copy, and the loose file game:\media\db\gamedb.slt
Mods: media/stringtables/en/main.str from ExampleTitleText: in place of the title update's copy
Mods: update:\media.zip holds 918 files (2 replaced, 0 added by mods; 1 loose file(s) replaced), 13.6 MB
Mods: the game reads media/stringtables/en/main.str (Example: title text)
```

A mod that cannot be loaded is skipped with the reason, and the game starts without it:
`Mods: skipped BrokenMod: cannot read media/gamemodes/unreadable.xml`, `folder not found`,
`no media folder`, `no files under media/`, a file over 2 GB. Files the game would not find are
left out with a warning (outside `media/`, non-ASCII names, whole `.zip` archives). When two mods
have the same file, the log says which one is used. A file that is in no archive yet is added
(`added (over any copy in the game's other archives)`); the game only reads it if something asks
for that path.

Edit a mod while the game is closed: the files are read when the game starts, and a file that
changes size while the game runs reads as zeros past its new end (logged).

## Known to work

Checked in the game (2026-10-24, Title Update 4 build, Linux):

| Mod | Result |
| --- | --- |
| `media/stringtables/en/main.str` with "PRESS START" changed (`ExampleTitleText`) | the title screen reads MODS LOADED; with `ExampleTitleText2` after it, LATER WINS! (load order) |
| `media/db/gamedb.slt` with the change of #117 (`ExampleGearbox`: a sixth gear for the Street transmission of the 1970 Boss 429 Mustang; the copy tested also had a car row added, one database page more) | the game starts and drives in free roam; its in-memory database holds the six-gear row (found in guest memory, and the original row in a run without the mod) |
| `media/renderscenarios/global.xml` (the disc's `renderscenarios.zip` member `Global.xml`, one setting changed; the title update has no copy) | added to the archive; the log shows the game reading the mod's copy at start |
| `media/stringtables/en/data_car.str` from the December IGN Car Pack's `StringTables_pri_1650.zip`, with a car's name changed, the DLC installed | the pause menu shows the changed name: a mod outranks the DLC's archives too |

Expected to work the same way, not checked one by one yet: any other member of the game's
archives (other XML settings such as `media/gametunablesettings/globalregistry.xml`, a car's files
under `media/cars/<car>/`, textures, sounds). Not supported: replacing a whole archive (`media/cars/X.zip`; put
the changed files from inside it in the mod instead: the game finds a disc archive's directory at
the offset `zipmanifest.xml` gives, so a different archive there would not be read).

## Why editing media.zip hung the game (#117)

Every member of the title update's `media.zip` carries an extra field (id `0x1123`) with the
absolute offset of its data, which the game reads instead of the member's local header. A zip tool
that replaces one member rewrites the archive and moves every member after it, but copies that
field unchanged. `db/gamedb.slt` is the twelfth of 918 members: when it changes size, the game
reads each of the 906 members after it at its old offset, wrong bytes for nearly everything it
loads. That is the hang on the very first
screen (the copyright text) of #117, and it does not depend on the database: the same archive
rebuilt with a modified `gamedb.slt` stored, the fields stale, hangs there; with the fields
corrected it plays. The database change itself was fine. A mod avoids it: the original members
never move, and the mod's members get fields of their own.

## How it works

The game mounts its archives into one tree by priority: the disc's archives (`zipmanifest.xml`,
priority 95 and 80), the DLC's (`Media\DLCZips\<name>_pri_<n>.zip`: 65 and 1650 in the packages
seen) and the title update's `update:\media.zip`, mounted at `game:\media\` with priority 100000
(TU4 `sub_82581DB8`). At start the runtime puts its own file in the place of
`update:\media.zip`: the update's archive byte for byte up to its central directory, then each mod
file as a stored member, then a new central directory (the original records, except those a mod
replaces, plus the mods' records with their `0x1123` fields) and the end record. Reads of it come
from the original archive and the mod files; nothing is copied or written. The game's own zip
reader then finds a mod's file wherever it would find the update's, ahead of every other archive
and loose file. The base game asks for `update:\media.zip` as well (and mounts it the same way); without a
title update it gets an empty archive to hold the mods. Source: `forza-recomp/src/platform/mod_archive.h` (the archive, the mod folders),
`forza-recomp/src/kernel/mod_overlay.cpp` (the runtime part), `launcher/forza_launcher/mods.py`
and `ui/mods_page.py` (the Mods page). Tests: `forza-recomp/tests/mod_archive_test.cpp`,
`launcher/test_mods.py`.

The alternative, hooking the game's archive functions (open, directory lookup, read), would be
two sets of addresses (base and TU4) and the game's LZX streams and caches to deal with; the
archive file needs neither.

## Limits (milestone 1)

- The base game (no title update) gets an empty `update:\media.zip` (in the cache folder,
  `mods_update`) for the mods; a file taken from the title update (its `gamedb.slt`, say) may
  not suit the base game's executable.
- A mod file is at most 2 GB, the archive at most 4 GB and 65,534 members (the game's zip reader
  has no Zip64).
- Online play does not check mods: in a session, players with different mods may see different
  things.
