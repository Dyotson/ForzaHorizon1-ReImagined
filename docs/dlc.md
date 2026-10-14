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

(in progress)

## Missing DLC content message (#9)

The text is `TitleUpdate2.str` 0x44E6: "You have cars in your garage that are missing DLC
content. These cars will be unavailable until you re-install the content or connect to
Xbox LIVE." (`Main.str` has an older variant). So it is a garage check (cars whose content
is not available), not a license check of packages. The string ids around it in the
executable: `IDS_MissingContent_DLC`, `IDS_MissingContent_Disc2`, `IDS_GarageMissingContent`
(`0x8209C8EC`).

(in progress)
