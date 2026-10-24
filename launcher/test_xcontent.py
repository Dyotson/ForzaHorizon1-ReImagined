"""The STFS reader checks every data block against the package's own hash table, so a damaged
package (issue #86: 12 blocks of the Rally pack's ColoradoDirt zip in an archive copy do not match
their hashes, nothing in the format can recover them) is reported instead of extracting silently:
python3 -m unittest launcher/test_xcontent.py"""
import hashlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import xcontent_extract as xc  # noqa: E402

HEADER = 0x2000


def build(data: bytes, damage_block=None, name="b.bin") -> bytes:
    """A read-only STFS package: hash table at block 0, the file table in data block 0 and the
    file `a/b.bin` in data blocks 1..n (chained through the hash records)."""
    n = (len(data) + 4095) // 4096
    blocks = [data[i * 4096:(i + 1) * 4096].ljust(4096, b"\0") for i in range(n)]
    entry = bytearray(0x40)
    encoded_name = name.encode("latin-1")
    entry[:len(encoded_name)] = encoded_name
    entry[0x28] = len(encoded_name)
    entry[0x2F:0x32] = (1).to_bytes(3, "little")
    entry[0x32:0x34] = struct.pack(">H", 0xFFFF)
    entry[0x34:0x38] = struct.pack(">I", len(data))
    table = bytes(entry).ljust(4096, b"\0")
    allb = [table] + blocks
    hashes = bytearray(4096)
    for i, b in enumerate(allb):
        nxt = i + 1 if 1 <= i < n else 0xFFFFFF
        hashes[i * 0x18:i * 0x18 + 20] = hashlib.sha1(b).digest()
        hashes[i * 0x18 + 0x14:i * 0x18 + 0x18] = struct.pack(">I", nxt)
    if damage_block is not None:
        allb[1 + damage_block] = bytes(x ^ 0xFF for x in allb[1 + damage_block])
    head = bytearray(HEADER)
    head[0:4] = b"LIVE"
    struct.pack_into(">I", head, 0x340, HEADER)
    head[0x379 + 2] = 1  # read only
    struct.pack_into("<H", head, 0x379 + 3, 1)  # file table blocks
    struct.pack_into(">II", head, 0x379 + 0x1C, len(allb), 0)
    return bytes(head) + bytes(hashes) + b"".join(allb)


class DamagedPackage(unittest.TestCase):
    def run_extract(self, pkg_bytes, size):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "pkg")
            Path(p).write_bytes(pkg_bytes)
            pkg = xc.open_package(p)
            try:
                (e,) = [e for e in pkg.entries() if not e.is_dir]
                bad = xc.extract(pkg, e, os.path.join(d, "out"))
                out = Path(d, "out", "b.bin").read_bytes()
                return bad, out
            finally:
                for f in pkg.files:
                    f.close()

    def test_intact_package_extracts_without_complaint(self):
        data = os.urandom(4096 * 3 + 100)
        bad, out = self.run_extract(build(data), len(data))
        self.assertEqual(bad, [])
        self.assertEqual(out, data)

    def test_blocks_that_fail_their_hash_are_reported_by_file_block(self):
        data = os.urandom(4096 * 3 + 100)
        bad, out = self.run_extract(build(data, damage_block=1), len(data))
        self.assertEqual(bad, [1])
        self.assertNotEqual(out, data)
        self.assertEqual(out[:4096], data[:4096])

    def test_damaged_partial_final_block_is_reported(self):
        for size in (100, 4096 * 3 + 100):
            with self.subTest(size=size):
                data = b"x" * size
                last_block = (size - 1) // 4096
                bad, out = self.run_extract(build(data, damage_block=last_block), size)
                self.assertEqual(bad, [last_block])
                self.assertEqual(len(out), size)
                self.assertNotEqual(out, data)

    def test_partial_block_hash_includes_stored_padding_without_extracting_it(self):
        data = b"x" * (4096 + 100)
        package = bytearray(build(data))
        package[-1] ^= 0xFF  # damage only padding, not the declared file contents
        bad, out = self.run_extract(bytes(package), len(data))
        self.assertEqual(bad, [1])
        self.assertEqual(out, data)

    def test_truncated_final_block_cannot_be_verified(self):
        data = b"x" * (4096 + 100)
        with self.assertRaisesRegex(SystemExit, "short read"):
            self.run_extract(build(data)[:-1], len(data))


class ExtractionPaths(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "output"
        self.output.mkdir()
        self.victim = self.root / "outside.bin"
        self.victim.write_bytes(b"keep existing data")
        self.pkg = SimpleNamespace(files=[io.BytesIO(b"x")])

    def test_stfs_parent_path_cannot_overwrite_sibling_file(self):
        package = self.root / "package"
        package.write_bytes(build(b"replacement", name="../outside.bin"))
        pkg = xc.open_package(package)
        try:
            entry, = [e for e in pkg.entries() if not e.is_dir]
            with self.assertRaisesRegex(SystemExit, "unsafe.*path"):
                xc.extract(pkg, entry, self.output)
            self.assertEqual(self.victim.read_bytes(), b"keep existing data")
        finally:
            for stream in pkg.files:
                stream.close()

    def test_absolute_parent_and_windows_path_forms_are_rejected(self):
        for path in (str(self.victim).replace("\\", "/"), "../outside.bin",
                     "nested/../../outside.bin", r"..\outside.bin", "C:outside.bin",
                     "file:stream", "nested/../alias.bin"):
            with self.subTest(path=path):
                self.pkg.files[0].seek(0)
                entry = xc.Entry(path, False, 1, [[0, 0, 1]])
                with self.assertRaisesRegex(SystemExit, "unsafe.*path"):
                    xc.extract(self.pkg, entry, self.output)
                self.assertEqual(self.victim.read_bytes(), b"keep existing data")

    def test_parent_directory_entry_cannot_create_an_outside_directory(self):
        entry = xc.Entry("../outside-dir", True, 0, [])
        with self.assertRaisesRegex(SystemExit, "unsafe.*path"):
            xc.extract(self.pkg, entry, self.output)
        self.assertFalse((self.root / "outside-dir").exists())

    def test_normal_nested_file_stays_beneath_output(self):
        entry = xc.Entry("Media/DLCZips/car.bin", False, 1, [[0, 0, 1]])
        self.assertEqual(xc.extract(self.pkg, entry, self.output), [])
        self.assertEqual((self.output / "Media/DLCZips/car.bin").read_bytes(), b"x")

    def test_existing_directory_link_cannot_redirect_output(self):
        target = self.root / "outside-dir"
        target.mkdir()
        link = self.output / "redirect"
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError:
            # Junctions need no Developer Mode/admin rights on Windows.
            if os.name != "nt" or subprocess.run(
                    ["cmd", "/c", "mklink", "/J", str(link), str(target)],
                    capture_output=True).returncode:
                self.skipTest("directory links unavailable")
        entry = xc.Entry("redirect/new.bin", False, 1, [[0, 0, 1]])
        with self.assertRaisesRegex(SystemExit, "unsafe.*path"):
            xc.extract(self.pkg, entry, self.output)
        self.assertFalse((target / "new.bin").exists())


if __name__ == "__main__":
    unittest.main()
