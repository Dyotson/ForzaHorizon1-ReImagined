# Installing from the retail disc (#23)

The launcher takes the base game from a Games on Demand package, an extracted folder, a bare
`default.xex`, or the retail **disc 1 as an ISO**. This page: the disc image format, what the
disc's executable is, how Title Update 4 applies to it, and what disc 2 is.

## ISO files: `tools/xiso_extract.py`

    tools/xiso_extract.py ISO info | list | extract OUTDIR [PATH ...]

Standard library only, read-only. An Xbox 360 game disc image is a 2048-byte-sector image
whose game partition is an XDVDFS volume (the file system `tools/xcontent_extract.py` already
reads inside a Games on Demand package; the directory code is shared, `Xdvdfs` there). The
volume starts at an offset that depends on the disc format; the volume descriptor
`MICROSOFT*XBOX*MEDIA` + root sector (u32 LE) + root size (u32 LE) is at sector 32 of the
volume. Offsets probed, in this order: `0x2080000` (XGD3), `0xFD90000` (XGD2), `0x18300000`
(XGD1), `0x2090000` (some XGD3 dumps), `0` (a plain XDVDFS image). Files are contiguous, so
a file is one run of bytes; truncated images are reported.

`installer.extract_game()` (launcher) recognises an ISO by that signature, not by its
extension, extracts it with the same progress/cancel interface as a package, and skips the
disc's `$SystemUpdate` folder (the console's system software update, 0.1 GB, not used).
The first file extracted is `default.xex`, which is checked before the long copy.

Tested: Forza Horizon (Europe, Asia), XGD3 (volume at `0x2080000`, 8.7 GB image). Extraction
takes seconds on an SSD.

## What is on disc 1

2486 entries: `default.xex` and the two facade DLLs (`XMediaFacade_default.xex`,
`SpeechFacade_default.xex`), `media/` (cars, tracks, audio, speech, UI zips, the game
database `media/db/gamedb.slt`), `nuisp*` (Kinect speech data), `nxeart`, `NuiIdentity.bin.be`
and `$SystemUpdate`.

Compared with a Games on Demand extraction of the base game (the project's `extracted/game`):
the same 2446 files with the same sizes. Byte differences:

- `default.xex`: the disc's header is laid out differently (see below) and six bytes of the
  image differ, all in one string in `.edata` (`0x83521156`): the name of the game database
  file. The decoded code is identical.
- `media/db/gamedb.slt`: 24 bytes: SQLite header fields (change counter, database size, version numbers), and in 16 rows of
  `Data_Car` one numeric field (9 on the disc, 10 in the other copy). Nothing else in any
  table differs.
- Everything else (the facades, all of `media/`) is byte-identical.

## The disc's `default.xex`

| Source | Version | Media id | Entry point | Header |
|---|---|---|---|---|
| Games on Demand package | 0.0.0.7 | `2B7A1346` | `0x82A6FBC0` | security info at `0x128` (0x80 bytes of zeros after the option table) |
| Disc 1, Europe/Asia, XGD3 | 0.0.0.7 | `2B7A1346` | `0x82A6FBC0` | security info at `0xA8`, the standard layout |

The issue assumed the disc carried 0.0.0.12 (the executable Title Update 4 updates). It does
not: this disc has the same version and code as the package, so the launcher accepted it
without any change (`installer.inspect_xex` checks title id, version and entry point; it
reads no hash). Codegen and the hints work on it unchanged.

Adding a version: `KNOWN_BASE` in `launcher/forza_launcher/installer.py` maps
`(version, entry point)` to a label. A default.xex that is not in the table is refused with
the version, base version, media id and entry point it has, and a request to report it.
The USA disc may have another media id (and possibly other code); check it by applying
Title Update 4 to it (below), which verifies the code page by page.

## Title Update 4 from the disc's executable

`tools/apply_title_update.py` works on the disc's `default.xex` directly, and better than
on the package's: it needs the header in the standard layout, which the disc has.

    default.xex  0.0.0.7 -> 0.0.4.12, 371 of 371 page groups match the update's digests
    XMediaFacade_default.xex  61 of 61;  SpeechFacade_default.xex  54 of 54

With the package's executable one page group (`0x83610000`, the page holding that database
file name string) did not match; with the disc's it does. The patched facades are
byte-identical to the ones made from the package; the patched `default.xex` differs in the
same six bytes.

Proof (2026-10-14): the launcher's headless path (`forza-launcher --extract ISO --root R`,
then `--build --root R --title-update <TU4 zip>`) ran in a scratch data root: codegen, a
full TU4 build at `-j4` from the disc's files (5.0 min, peak 1.9 GB), and a boot to the main
menu, which lists SINGLE PLAYER, MULTIPLAYER and HORIZON RALLY (scratch profile, offline).
A disc image cut short, a file that is not an image and a missing file are refused with a
message (`tools/xiso_extract.py` names a truncated image).

## Disc 2

Findings from disc 1 (no disc 2 image was available; nothing below was checked against one):

- The game knows two discs. Strings: `IDS_Reading_Disc1`, `IDS_Reading_Disc2`,
  `IDS_InsertDisc1` ("PLEASE INSERT DISC 1", `Disc2Strings.str`), `IDS_MissingContent_Disc2`,
  `DISC2_LOCKED`/`DISC2_UNLOCKED`; SQL functions `IsCarOnDisc`, `IsCarDisc1`,
  `IsEnvironmentOnDisc`, `IsShowroomCarOnDisc`.
- `gamedb.slt` has a `MultiDiscContentPacks` table with three packs, content ids
  `4D5309100CCF0001` Asian Car Pack, `...0002` European Car Pack and `...0003` American and
  British Car Pack (the strings give "28 cars", "39 cars" and "37 cars", 104 in all), each with a slide image
  (`media/ui/textures/ContentPacks/*.tga`) and `IsDLC = 0`, next to two placeholder packs
  `...FFFE`/`...FFFF` (`IsDLC = 1`; 52 cars of `Data_Car` carry `ContentId = 99`, the
  placeholder). Disc 2 is therefore, most likely, the three **car packs**: about 100 cars.
- The executable builds the path `game:\Content\0000000000000000\<title id>\00000002\<16 hex
  digits>` (`0x8200D8E8`, used at `sub_824E365C`): a marketplace-content (type 2) package in a
  `Content` folder on the game device, named by the pack's content id. That is the format of
  the DLC packages `tools/install_dlc.py` installs (STFS, `00000002`), so disc 2 most likely
  carries `Content/0000000000000000/4D5309C9/00000002/4D5309100CCF000{1,2,3}` as STFS
  packages, which the console copies to the hard drive.
- What the game does without it: the lobby content check (`sub_825A56E0`, returns 3 for a car
  the player does not have) and `sub_826B4200` pick the "missing content" message (DLC,
  Disc 2 or unknown) by the car's pack. All 176 cars of
  `Data_Car` have their files on disc 1 (`media/cars/*.zip`) and `IsInstalled = 1`, so
  nothing in disc 1's database needs disc 2 to reach the menu or free roam. The pack cars
  do not seem to be in that table; a garage or a lobby that holds one is reported as missing content
  ("Disc 2" when the car belongs to these packs).

Not implemented: installing disc 2 (ISO or folder). The plan, once an image is at hand:
list it with `tools/xiso_extract.py DISC2 list`; if `Content/0000000000000000/4D5309C9/
00000002/*` are STFS packages, extract that subtree and run each through
`install_dlc.install()` (as the DLC folder step does; `installer.install_dlc_folder` takes
the extracted `00000002` folder as it is), then check with `FORZA_DLC_LOG=1` that
`IsCarInstalled` answers 1 for the pack cars. If the layout differs, adjust the path above.
`installer.extract_game` already refuses a disc without `default.xex` with a message that
asks for disc 1.
