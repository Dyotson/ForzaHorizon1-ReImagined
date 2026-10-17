"""The STFS reader checks every data block against the package's own hash table, so a damaged
package (issue #86: 12 blocks of the Rally pack's ColoradoDirt zip in an archive copy do not match
their hashes, nothing in the format can recover them) is reported instead of extracting silently:
python3 -m unittest launcher/test_xcontent.py"""
import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import xcontent_extract as xc  # noqa: E402

HEADER = 0x2000


def build(data: bytes, damage_block=None) -> bytes:
    """A read-only STFS package: hash table at block 0, the file table in data block 0 and the
    file `a/b.bin` in data blocks 1..n (chained through the hash records)."""
    n = (len(data) + 4095) // 4096
    blocks = [data[i * 4096:(i + 1) * 4096].ljust(4096, b"\0") for i in range(n)]
    entry = bytearray(0x40)
    entry[:5] = b"b.bin"
    entry[0x28] = 5
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
            (e,) = [e for e in pkg.entries() if not e.is_dir]
            bad = xc.extract(pkg, e, os.path.join(d, "out"))
            out = Path(d, "out", "b.bin").read_bytes()
            for f in pkg.files:
                f.close()
            return bad, out

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


if __name__ == "__main__":
    unittest.main()
