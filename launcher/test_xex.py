"""Tests of the default.xex acceptance and package picking (python3 -m unittest launcher/test_xex.py
from the repository root). They need the supported game files (FORZA_TEST_GAME, default
extracted/game) and skip without; the Title Update 4 test needs FORZA_TEST_TU4 (a folder with its
patched default.xex, default extracted/tu4). Scratch files go to FORZA_TEST_TMP (default
~/.cache/forza-xexver); nothing is written next to the originals."""
import hashlib
import os
import shutil
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
TMP = Path(os.environ.get("FORZA_TEST_TMP", Path.home() / ".cache" / "forza-xexver"))
os.environ["XDG_CONFIG_HOME"] = str(TMP / "config")   # the verdict cache goes here
sys.path.insert(0, str(HERE))

from forza_launcher import installer, xexcheck  # noqa: E402

GAME = Path(os.environ.get("FORZA_TEST_GAME", ROOT / "extracted" / "game"))
TU4 = Path(os.environ.get("FORZA_TEST_TU4", ROOT / "extracted" / "tu4"))
FILES = installer.REQUIRED_FILES


def set_header(path, version, base, media):
    raw = bytearray(path.read_bytes())
    opts = {}
    for i in range(struct.unpack_from(">I", raw, 0x14)[0]):
        k, v = struct.unpack_from(">II", raw, 0x18 + i * 8)
        opts[k] = v
    struct.pack_into(">III", raw, opts[0x40006], media, version, base)
    path.write_bytes(raw)


def write_modified(src, dst, edit):
    """A copy of the executable src whose image was changed by edit(image) and whose page digest
    chain was recomputed (so only the code differs; the file is intact). Stored plain."""
    atu = xexcheck.atu
    x = atu.Xex(src.read_bytes(), src.name)
    image = x.image()
    edit(image)
    header = bytearray(x.raw[:x.header_size])
    descs = x.page_descriptors()
    ps = x.page_size()
    offs, off = [], 0
    for v, _ in descs:
        offs.append(off)
        off += (v >> 4) * ps
    # chain: stored[i-1] = SHA1(pages_i + desc_i(v, stored[i])), stored[last] unchanged
    stored = [d for _, d in descs]
    for i in range(len(descs) - 1, 0, -1):
        v = descs[i][0]
        h = hashlib.sha1(bytes(image[offs[i]:offs[i] + (v >> 4) * ps]))
        h.update(struct.pack(">I", v) + stored[i])
        stored[i - 1] = h.digest()
    v0 = descs[0][0]
    h = hashlib.sha1(bytes(image[:(v0 >> 4) * ps]))
    h.update(struct.pack(">I", v0) + stored[0])
    header[x.sec + 0x114:x.sec + 0x128] = h.digest()
    for i, (v, _) in enumerate(descs):
        struct.pack_into(">I20s", header, x.sec + 0x184 + i * 24, v, stored[i])
    atu.write_xex(str(dst), bytes(header), image)


@unittest.skipUnless((GAME / "default.xex").is_file(), "no supported game files")
class Acceptance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        shutil.rmtree(TMP / "t", ignore_errors=True)
        (TMP / "t").mkdir(parents=True)

    def folder(self, name, version=None, media=None):
        d = TMP / "t" / name
        d.mkdir()
        for n in FILES:
            shutil.copy(GAME / n, d / n)
            if version is not None:
                set_header(d / n, version, version, media)
        return d

    def test_supported(self):
        ok, msg = installer.check_game_folder(GAME)
        self.assertTrue(ok, msg)
        self.assertNotIn("different printing", msg)

    def test_other_printings_with_the_same_code(self):
        for version, media in ((9, 0x6368CD69), (10, 0x2DC7007B), (12, 0x4000D145), (11, 0x11111111)):
            d = self.folder(f"v{version}", version, media)
            ok, msg = installer.check_game_folder(d)
            self.assertTrue(ok, msg)
            self.assertIn("different printing", msg)
            self.assertIn(f"{media:08X}", msg)

    def test_title_update_is_refused_as_such(self):
        if not (TU4 / "default.xex").is_file():
            self.skipTest("no patched default.xex")
        ok, msg = installer.inspect_xex(TU4 / "default.xex")
        self.assertFalse(ok)
        self.assertIn("title update is already applied", msg)
        self.assertIn("0.0.4.12", msg)

    def test_changed_code_is_refused_naming_the_section(self):
        d = self.folder("code", 12, 0x4000D145)
        x = xexcheck.atu.Xex((GAME / "default.xex").read_bytes())
        text = next(s for s in xexcheck._sections(x.image()) if s[0] == ".text")

        def edit(image):
            image[text[1] + 0x12345] ^= 1
        write_modified(GAME / "default.xex", d / "default.xex", edit)
        set_header(d / "default.xex", 12, 12, 0x4000D145)
        ok, msg = installer.check_game_folder(d)
        self.assertFalse(ok)
        self.assertIn("section .text (code) at 0x82", msg)
        self.assertIn("is not the supported code", msg)

    def test_changed_data_section_is_refused_too(self):
        d = self.folder("data", 9, 0x6368CD69)
        x = xexcheck.atu.Xex((GAME / "default.xex").read_bytes())
        sec = next(s for s in xexcheck._sections(x.image()) if s[0] == ".data")

        def edit(image):
            image[sec[1] + 0x100] ^= 1
        write_modified(GAME / "default.xex", d / "default.xex", edit)
        set_header(d / "default.xex", 9, 9, 0x6368CD69)
        ok, msg = installer.check_game_folder(d)
        self.assertFalse(ok)
        self.assertIn("section .data (data) at 0x83", msg)

    def test_padding_outside_the_sections_is_ignored(self):
        # the printings differ in a string in the padding after .edata (0x83521156)
        d = self.folder("pad", 12, 0x4000D145)

        def edit(image):
            image[0x1521156:0x1521160] = b"othername\0"
        write_modified(GAME / "default.xex", d / "default.xex", edit)
        set_header(d / "default.xex", 12, 12, 0x4000D145)
        ok, msg = installer.check_game_folder(d)
        self.assertTrue(ok, msg)

    def test_damaged_file_is_said_to_be_damaged(self):
        d = self.folder("damaged", 12, 0x4000D145)
        raw = bytearray((d / "default.xex").read_bytes())
        raw[0x500000] ^= 0xFF   # inside the encrypted payload: the digests no longer match
        (d / "default.xex").write_bytes(raw)
        ok, msg = installer.check_game_folder(d)
        self.assertFalse(ok)
        self.assertIn("damaged", msg)

    def test_facade_with_other_code_is_refused(self):
        d = self.folder("facade", 12, 0x4000D145)
        f = "XMediaFacade_default.xex"
        x = xexcheck.atu.Xex((GAME / f).read_bytes())
        sec = next(s for s in xexcheck._sections(x.image()) if s[3] & 0x20000000)

        def edit(image):
            image[sec[1] + 0x100] ^= 1
        write_modified(GAME / f, d / f, edit)
        set_header(d / f, 12, 12, 0x4000D145)
        ok, msg = installer.check_game_folder(d)
        self.assertFalse(ok)
        self.assertIn(f, msg)

    def test_other_game_and_patch_files(self):
        d = self.folder("other", 12, 0x4000D145)
        raw = bytearray((d / "default.xex").read_bytes())
        opts = {}
        for i in range(struct.unpack_from(">I", raw, 0x14)[0]):
            k, v = struct.unpack_from(">II", raw, 0x18 + i * 8)
            opts[k] = v
        struct.pack_into(">I", raw, opts[0x40006] + 12, 0x4D5309C8)
        (d / "default.xex").write_bytes(raw)
        ok, msg = installer.inspect_xex(d / "default.xex")
        self.assertFalse(ok)
        self.assertIn("not Forza Horizon", msg)


class Packages(unittest.TestCase):
    def test_data_file_or_folder_resolves_to_the_package(self):
        shutil.rmtree(TMP / "p", ignore_errors=True)
        base = TMP / "p" / "4D5309C9" / "00007000"
        (base / "ABCDEF.data").mkdir(parents=True)
        (base / "ABCDEF").write_bytes(b"x")
        (base / "ABCDEF.data" / "Data0000").write_bytes(b"y")
        (base / "ABCDEF.data" / "Data0001").write_bytes(b"y")
        for pick in (base / "ABCDEF", base / "ABCDEF.data", base / "ABCDEF.data" / "Data0000",
                     base / "ABCDEF.data" / "Data0001", base):
            self.assertEqual(installer.resolve_package(str(pick)), base / "ABCDEF", pick)
        (base / "ABCDEF").unlink()
        with self.assertRaises(installer.InstallError) as c:
            installer.resolve_package(str(base / "ABCDEF.data" / "Data0000"))
        self.assertIn("ABCDEF", str(c.exception))


if __name__ == "__main__":
    unittest.main()
