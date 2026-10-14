# DLC and Title Update 4

Status of the downloadable content (issues #5, #9, #16, #17, #18) and of Title Update 4
(#15). Newest findings first within each section.

## Title Update 4

### What the update is

`Forza Horizon (World) (v4) (4000D145) (Title Update)`: an STFS package, content type
`0xB0000` (`4D5309C9/000B0000/tu00000001_00000000`), media id `4000D145`, base version 12.
Files: `default.xexp` (2.8 MB), `XMediaFacade_default.xexp`, `SpeechFacade_default.xexp`
(delta patches, module flags `0x50`, compression type 3) and `media.zip` (10.9 MB).

The executable it updates (`0.0.0.12` -> `0.0.4.12`) is not byte-for-byte the one in this
project (`0.0.0.7`, media id `2B7A1346`): the patch's source signature hash does not match,
and the header delta copies bytes from a header laid out differently (security info right
after the optional header table, the header data back to back; this copy has 0x80 bytes of
zeros after the table, 16-byte aligned header data and a zeroed RSA signature). The code is
the same: with the header repacked to the original layout, the update's block hashes pass
and 370 of the 371 page groups of the patched image match the update's page digests. The
one that does not is `0x83610000-0x8361FFFF` (in `.edata`'s page, copied unchanged from
this copy's `0x83520000`: data, not code). The facades patch with every page group
verified.

### Applying it: `tools/apply_title_update.py`

    tools/apply_title_update.py BASE_XEX TITLE_UPDATE OUT_XEX

`TITLE_UPDATE` is the package, a zip holding it, a folder, or a bare `.xexp`. Writes the
patched `default.xex` (unencrypted, one "basic" block), the patched facade DLLs next to it,
and the update's other files under `OUT_DIR/update/` (the folder to mount as `update:`).
Pure Python (`tools/lzx_delta.py` is a port of libmspack's LZX decoder with a preloaded
window; AES uses `cryptography` or `openssl` when present). About 2 s.

How it checks itself: the patched headers must parse with consistent sizes, every patch
block must match its SHA-1 (so the image key is right), and the result is checked against
the XEX page digest chain (`SHA-1(pages of descriptor i + descriptor i)` is stored in
descriptor i-1, descriptor 0's in the security info's section digest). Digests the header
delta copied from the other copy's headers are rebuilt from the verified pages; ranges that
still fail are reported, and the tool refuses when one is in code.

Notes:
- ReXGlue's codegen already applies a `default.xexp` that sits next to the input XEX
  (`UserModule::LoadFromFile` looks for a sibling `.xexp`), but through
  `XexModule::ApplyPatch`, which (from Xenia) bounds the header records by the first
  record's length: right for `default.xexp` (one record), wrong for the facades' patches
  (several), and it fails on this copy's headers anyway. The pre-patched XEX avoids both.
- The base game already asks for `update:\media.zip` (Xenia's log: "ResolvePath(update:\
  media.zip) failed"). The SDK mounts `update:` from its `update_data_root` setting.

### Building it

    python3 tools/apply_title_update.py extracted/game/default.xex <TU package or zip> extracted/tu4/default.xex
    cmake -S forza-recomp -B forza-recomp/out/build/linux-tu4 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64 \
      -DFORZA_TITLE_UPDATE=4
    FORZA_BUILD=linux-tu4 tools/build.sh
    FORZA_BUILD=linux-tu4 tools/play.sh

- `-DFORZA_TITLE_UPDATE=4` picks `forza_manifest_tu4.toml` (input `extracted/tu4/`, hints in
  `hints_tu4/`, output `generated_tu4/`; codegen rewrites `generated/rexglue.cmake` with
  fixed paths each run, so CMake includes a copy pointed at `generated_tu4/`). Both
  versions build from the same tree into separate build folders; the base build is
  unchanged.
- Hints: `tools/tu_address_map.py` maps base addresses to TU4 ones (functions paired
  through both images' `.pdata`, aligned in order with difflib on normalized code; changed
  functions paired by similarity within their gap; data addresses through the lis/addi
  pairs of matched code). It ported every file of `hints/` to `hints_tu4/` (entries it could
  not map are commented out), then `tools/find_missing_entries.py` (+87) and
  `tools/find_orphan_functions.py` (+202) were run on the TU4 output until they found
  only what they find on the base (5 and 8 rejects). `hints_tu4/tu4_fixes.toml`: one
  function end (`sub_827611D8`) the scanner cut short.
- Hooks: sources name guest functions and addresses as in the base executable.
  `src/tu4_addresses.h` (written by `tu_address_map.py --cpp forza-recomp/src ...`)
  renames each `sub_XXXXXXXX`/`__imp__sub_XXXXXXXX` they use and maps each
  `FORZA_ADDR(0x...)` constant; `src/platform/guest_addr.h` includes it in a TU4 build.
  Entries marked "check" sit in functions the update changed (13 functions, 5 call-site
  addresses, all in the online lobby / Horizon World hooks and the frame-time hook); they
  were paired by similarity and are untested.
- Runtime: the TU4 build mounts the patched files' folder as `update:` (the SDK's
  `update_data_root`, default `tu4` next to the game folder) and links
  `game:\default.xex` and the two facades to it. TU4 looks the facades' exports up
  again and again; the SDK made a new thunk for each lookup and ran out within a second,
  so `src/kernel/proc_address_cache.cpp` gives each (module, ordinal, caller) its first
  thunk again.
- Cost: codegen about 70 s, a full build about 15 minutes at -j4 on this machine.

### Status

- TU4 reaches the main menu, which lists HORIZON RALLY next to Single Player and
  Multiplayer (the base has no such entry). With a profile without progress the game
  says "You must earn a yellow wristband in Horizon Single Player before you can enter
  Horizon Rally" (game rule). With a Xenia save, Horizon Rally loads its map and resumes
  the rally progress; the free roam pause menu lists 1000 CLUB, which says "not available
  because you are not signed into Xbox LIVE" when offline; signed in (Forza Live
  server) it opens (tutorial, then the challenge cards).
- Saves: TU4 converts a profile save when it loads it (`ForzaProfile/VersionFlags` goes
  from 0 to version 4, flags 2; a `NeedsAA` file appears). The base build then refuses it:
  "MISSING TITLE UPDATE: Your profile save game requires the latest title update". Going
  to TU4 is one way: keep a copy of the profile folder to go back.

## Missing DLC content message (#9)

The text is `TitleUpdate2.str` 0x44E6: "You have cars in your garage that are missing DLC
content. These cars will be unavailable until you re-install the content or connect to
Xbox LIVE." (`Main.str` has an older variant). So it is a garage check (cars whose content
is not available), not a license check of packages. The string ids around it in the
executable: `IDS_MissingContent_DLC`, `IDS_MissingContent_Disc2`, `IDS_GarageMissingContent`
(`0x8209C8EC`).

(in progress)

How it is decided (base addresses): the profile load handler `sub_827A1CD0` shows it
(`TitleUpdate2`, id `0x83310F54` = `IDS_GarageMissingContent`) when a vtable check on the
profile is false and `sub_82500AD8(profile)` is true. That runs
`SELECT COUNT() FROM <profile>Career_Garage WHERE IsCarInstalled(CarId)=0 AND
IsCarOnDisc(CarId)=0` on the game database. The SQL functions are registered in
`sub_8250B9xx`: `IsCarInstalled` = `sub_82502DD8`, `IsCarInstalledAndPurchased` =
`sub_82502E18`, `IsCarOnDisc` = `sub_824F1A80` (both installed checks go through a cache,
`sub_82502CB8`). So the message means: the save's garage holds a car whose content is
neither on the disc nor installed.
