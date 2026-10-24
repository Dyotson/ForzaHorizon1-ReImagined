#!/usr/bin/env python3
"""Make example mods from your own game files, for trying the mod loader (#149, docs/modding.md).

Usage: tools/make_example_mods.py UPDATE_MEDIA_ZIP OUT_MODS_DIR [--only NAME ...]

UPDATE_MEDIA_ZIP is the title update's media.zip of your installation (the launcher's
<data>/game/update/media.zip, or the folder tools/apply_title_update.py wrote). Each example is
a mod folder in OUT_MODS_DIR (which must not hold a folder of that name yet):

  ExampleTitleText   media/stringtables/en/main.str with the title screen's "PRESS START"
                     reading "MODS LOADED" (English).
  ExampleTitleText2  the same file reading "LATER WINS!": load it after ExampleTitleText to see
                     the load order at work.
  ExampleGearbox     media/db/gamedb.slt with the change of issue #117: the Street transmission
                     of the 1970 Boss 429 Mustang (List_UpgradeDrivetrainTransmission 157001)
                     with a sixth gear.

Members compressed with the game's LZX (zip method 21) are decoded with tools/unxmem (built
from tools/unxmem.c). Nothing of the game is in this script; the mods hold changed copies of
your own files, so keep them to yourself.
"""
import argparse
import sqlite3
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXAMPLES = ("ExampleTitleText", "ExampleTitleText2", "ExampleGearbox")


def member(zip_path: Path, name: str, unxmem: Path) -> bytes:
    """The bytes of member `name` (stored or LZX), checked against its CRC."""
    with zipfile.ZipFile(zip_path) as z:
        info = z.getinfo(name)
        with open(zip_path, "rb") as f:
            f.seek(info.header_offset)
            header = f.read(30)
            if header[:4] != b"PK\x03\x04":
                raise ValueError(f"{name}: no local header")
            n, m = struct.unpack("<HH", header[26:30])
            f.seek(info.header_offset + 30 + n + m)
            raw = f.read(info.compress_size)
    if info.compress_type == 0:
        data = raw
    elif info.compress_type == 21:
        if not unxmem.is_file():
            raise FileNotFoundError(f"{unxmem} not found: build it from tools/unxmem.c (see there)")
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "in.lzx", Path(tmp) / "out.bin"
            src.write_bytes(raw)
            subprocess.run([str(unxmem), str(src), str(out), str(info.file_size)], check=True,
                           stdout=subprocess.DEVNULL)
            data = out.read_bytes()
    else:
        raise ValueError(f"{name}: compression method {info.compress_type} not handled")
    if zipfile.crc32(data) & 0xFFFFFFFF != info.CRC or len(data) != info.file_size:
        raise ValueError(f"{name}: decoded bytes do not match the archive's CRC")
    return data


def write_mod(out: Path, folder: str, manifest: dict, files: dict):
    root = out / folder
    if root.exists():
        raise FileExistsError(f"{root} exists already")
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    lines = [f'{k} = "{v}"' for k, v in manifest.items()]
    (root / "mod.toml").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(root)


def title_text(media_zip: Path, unxmem: Path, text: str) -> bytes:
    data = member(media_zip, "stringtables/en/main.str", unxmem)
    old = "PRESS START".encode("utf-16-be") + b"\0\0"
    new = text.encode("utf-16-be") + b"\0\0"
    if len(new) != len(old) or data.count(old) != 1:
        raise ValueError("main.str does not hold PRESS START once (another language or version?)")
    return data.replace(old, new)


def gearbox(media_zip: Path, unxmem: Path) -> bytes:
    data = member(media_zip, "db/gamedb.slt", unxmem)
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp) / "gamedb.slt"
        db.write_bytes(data)
        c = sqlite3.connect(db)
        row = c.execute("SELECT NumGears, GearRatio4, GearRatio5 FROM List_UpgradeDrivetrainTransmission "
                        "WHERE Id = 157001").fetchone()
        if row is None or row[0] != 5:
            raise ValueError(f"transmission 157001 is not the expected 5-speed: {row}")
        # As in #117: one more gear, below the fifth's ratio.
        c.execute("UPDATE List_UpgradeDrivetrainTransmission SET NumGears = 6, GearRatio5 = ? "
                  "WHERE Id = 157001", (round(row[1] * 0.82, 2),))
        c.commit()
        # The game's SQLite (3.6/3.7) reads only the rollback-journal format: keep it so.
        mode = c.execute("PRAGMA journal_mode").fetchone()[0]
        c.close()
        if mode.lower() == "wal":
            raise ValueError("the database is in WAL mode, which the game cannot read")
        return db.read_bytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("media_zip", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--only", nargs="*", choices=EXAMPLES)
    ap.add_argument("--unxmem", type=Path, default=HERE / "unxmem")
    a = ap.parse_args()
    wanted = a.only or EXAMPLES
    try:
        if "ExampleTitleText" in wanted:
            write_mod(a.out, "ExampleTitleText",
                      {"name": "Example: title text", "version": "1.0", "author": "make_example_mods.py",
                       "description": "The title screen reads MODS LOADED instead of PRESS START (English)."},
                      {"media/stringtables/en/main.str": title_text(a.media_zip, a.unxmem, "MODS LOADED")})
        if "ExampleTitleText2" in wanted:
            write_mod(a.out, "ExampleTitleText2",
                      {"name": "Example: title text 2", "version": "1.0", "author": "make_example_mods.py",
                       "description": "The same file as Example: title text, reading LATER WINS!"},
                      {"media/stringtables/en/main.str": title_text(a.media_zip, a.unxmem, "LATER WINS!")})
        if "ExampleGearbox" in wanted:
            write_mod(a.out, "ExampleGearbox",
                      {"name": "Example: six-speed Boss 429", "version": "1.0", "author": "make_example_mods.py",
                       "description": "The Street transmission of the 1970 Boss 429 Mustang gets a sixth gear (#117)."},
                      {"media/db/gamedb.slt": gearbox(a.media_zip, a.unxmem)})
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, sqlite3.Error, subprocess.CalledProcessError) as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
