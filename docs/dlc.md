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
  the paths of the manifest that ran, so each build includes its own copy, made at
  configure time; after a TU4 codegen run git shows that file changed: `git checkout` it). Both
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

### For the launcher

- `apply_title_update.py BASE_XEX TU_PACKAGE OUT_XEX` writes `default.xex`, both facades
  and `media.zip` into OUT_XEX's folder; that folder is the game's `update:`.
- Build with `-DFORZA_TITLE_UPDATE=4` from `forza_manifest_tu4.toml`, rewriting its
  `../extracted/tu4` paths (the three `file_path`s and `game_root`) to that folder. Not
  `forza_manifest.toml` with the patched XEX: the base hints are at base addresses.
- Run with `--update_data_root=<that folder>` (the default is `tu4` next to the game
  folder). `--game_data_root` stays the disc's files.

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

## Every package (#18)

The 23 installed packages (`tools/install_dlc.py`), and what the game answers for their
cars (`FORZA_DLC_LOG=1`, `src/debug/dlc_log.cpp`: each IsCarInstalled /
IsCarInstalledAndPurchased / IsCarOnDisc answer is logged once). Car lists come from each
package's database patch (`media/db/patch/*_merge.slt` in its DLC zips); single-car
packages map their offer to a car of the disc's database (`ContentOffersMapping`).
Measured 2026-10-14 with all 23 installed, signed in to the Forza Live server: base build
with the normal profile, TU4 build with a Xenia save, each into free roam.

| Package | Offer | Cars (media names) | Base | TU4 |
|---|---|---|---|---|
| 1000 Club Expansion Pack | 2700 | 2: RUF_CTR2_95, FOR_F100_56 | installed | installed |
| 2010 Ferrari 458 Italia | 1002 | 1: FER_F142_10 | installed | installed |
| 2010 Nissan 370Z | 1004 | 1: NIS_370Z_10 | installed | installed |
| 2010 Volkswagen Golf R | 1005 | 1: VW_GolfR_10 | installed | installed |
| 2011 Mercedes-Benz SLS AMG | 1001 | 1: MER_SLSAMG_11 | installed | installed |
| 2012 Aston Martin Virage | 1003 | 1: AST_Virage_12 | installed | installed |
| 2013 Ford Mustang Boss 302  - Pizza Hut Edition | 2800 | 1: FOR_MustangBoss302_PZA_13 | installed | installed |
| 2013 Ford Shelby GT500 - Rockstar Energy | 1300 | 1: FOR_ShelbyGT500LCE_13 | installed | installed |
| April TopGear Car Pack | 2600 | 6: FOR_TransitSSV_11, BOW_NemesisEXR_12, ALF_ZagatoTZ3Stradale_12, BMW_M3GTR_HZN_05, AST_Vanquish_13, MER_CLKGTR_HZN_98 | installed | installed |
| December IGN Car Pack | 1500 | 7: AST_DBS_09, AMC_JavelinAMX_71, MCL_F1_93, FER_F12_13, FOR_GTX1_06, HUM_H1AlphaOpenTop_06, FOR_SVTRaptorHALO_12 | installed | installed |
| February Jalopnik Car Pack | 1900 | 7: NIS_510_70, DEV_GTX_10, FOR_CountrySquire_66, MAZ_RX3_73, TOY_FJCruiser_12, SUB_BRZ_13, FER_512TR_HZN_91 | installed | installed |
| Honda Challenge Car Pack | 1800 | 3: HON_CivicSi_13, HON_HPDRallyFit_10, HON_CivicSi_86 | installed | installed |
| January Recaro Car Pack | 1700 | 7: MER_SLR_05, PAG_ZondaCinque_09, FOR_Mustang_95, LEX_LFANurburgring_12, LOT_ExigeS_12, GMC_Vandura_83, CAD_Escalade_12 | installed | installed |
| LCE: Day1 DLC Pack | 1200 | 11: CHE_BelAir_57, FOR_MustangBoss302_13, AUD_LCESportQuattro_83, VW_LCESciroccoR_11, FOR_LCESVTRaptor_12, MIT_LCELancerEvoXGSR_08, DOD_LCEChargerSRT8_12, LAM_AventadorJ_12, ALF_8CSpider_10, FER_599XXEvolution_12, GUM_ApolloEnraged_12 | installed | installed |
| March Meguiar's Car Pack | 2400 | 7: PEU_206RC_04, JOS_JT1_10, BMW_M135i_13, FER_360Challenge_HZN_03, NIS_R390_HZN_98, AUD_RS4Avant_13, MER_G65AMG_13 | installed | installed |
| November Bondurant Car Pack | 1400 | 7: SHE_Cobra427_65, NIS_240SX_94, JEE_Wrangler_12, BMW_M3GTS_10, LAM_GallardoLP570Spyder_12, SHE_1000_12, FOR_ShelbyGT500LCE_13 | installed | installed |
| October Car Pack | 1100 | 11: CHE_BelAir_57, FOR_MustangBoss302_13, AUD_LCESportQuattro_83, VW_LCESciroccoR_11, FOR_LCESVTRaptor_12, MIT_LCELancerEvoXGSR_08, DOD_LCEChargerSRT8_12, LAM_AventadorJ_12, ALF_8CSpider_10, FER_599XXEvolution_12, GUM_ApolloEnraged_12 | installed | installed |
| Pre-Order Car Pack | 2500 | 5: NIS_370Z_10, FER_F142_10, MER_SLSAMG_11, VW_GolfR_10, AST_Virage_12 | installed | installed |
| Rally Expansion Pack | 1600 | 16: FOR_FocusSVT_03, SUB_WRXSTi_05, MIT_LancerEvoVIIIMR_04, FOR_EscortRSCosworth_92, LAN_037Stradale_82, TOY_CelicaST185_90, SUB_WRXSTi_05, AUS_MiniCooperS_65, TOY_Sprinter_85, PEU_205T16_84, LAN_DeltaS4_86, AUD_RS4_06, FOR_RS200Evolution_87, LAM_LP7004_12, ULT_GTR_12, FER_FXX_05 | installed | installed |
| Season Pass | 206931968 | 0:  | - | - |
| Season Pass: 2006 Lamborghini Miura Concept | 1450 | 1: LAM_Miura_06 | installed | installed |
| VIP Membership & Cars Pack | 1006 | 5: PAG_Huayra_12, LAM_SestoElemento_11, RUF_Rt12R_11, KOE_Agera_11, AUD_R8GTSpyder_12 | installed | installed |
| VIP Membership & Cars Pack | 2100 | 5: LAM_SestoElemento_11, KOE_Agera_11, PAG_Huayra_12, RUF_Rt12R_11, AUD_R8GTSpyder_12 | installed | installed |

- Every DLC car counts as installed on both builds, the Rally and 1000 Club cars too
  (the base merges their database patches; TU4's own `gamedb.slt`, in its media.zip,
  already has the 15 Rally cars and the 1600 and 2700 offers). What the expansions add
  besides cars is TU4 code: the Horizon Rally mode and its map (#16) and the 1000 Club
  (#17) exist only in the TU4 build.
- The 12 packages of the second DLC set (1000 Club, Rally, the car packs, VIP) are byte
  for byte the installed ones; `install_dlc.py` needs no change.
- Not checked one by one: each car in the Autoshow and garage (model, audio, thumbnail).
- The failed `DLCRoot` lookups in the log (#5) are the game probing every package for
  optional files (`DLCRootN:\media\dlczips\stringtables.xml`, per-zip
  `media\db\patch\`, `effects\`, `dynamicpost\...` folders): not errors.

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

Not reproduced (2026-10-14): base build with the normal profile and with the Xenia save,
signed in or not, START pressed at once or after the attract screens; TU4 build with the
Xenia save. Every IsCarInstalled answer was 1 (`FORZA_DLC_LOG=1`). The message can only
come from a garage car the game does not count as installed: a car from a package that is
not installed in the user data folder the game uses (`<user data>/0000000000000000/
4D5309C9/00000002`; `play.sh --xenia` and the launcher link it into the Xenia copy), or a
package installed without its header (`Headers/00000002/<name>.header`). With
`FORZA_DLC_LOG=1` the log names the car: a line `IsCarInstalled(id N, type 1) = 0`.
