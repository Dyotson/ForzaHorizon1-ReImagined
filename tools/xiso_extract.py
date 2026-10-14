#!/usr/bin/env python3
"""Read-only Xbox 360 disc image (ISO) lister/extractor (standard library only).

A disc image holds an XDVDFS volume (the same file system as inside a Games on Demand
package, see xcontent_extract.Xdvdfs) at one of the offsets the disc formats use:
0 (a plain XDVDFS image), 0x2080000 (XGD3), 0xFD90000 (XGD2), 0x18300000 (XGD1), and
0x2090000 (some XGD3 dumps). The volume descriptor ("MICROSOFT*XBOX*MEDIA", root sector,
root size) sits at sector 32 of the volume. The image is only ever opened for reading.

Usage:
  xiso_extract.py ISO info
  xiso_extract.py ISO list
  xiso_extract.py ISO extract OUTDIR [PATH ...]   (no PATH = everything)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xcontent_extract as xc  # noqa: E402

SECTOR = 0x800
# Where the XDVDFS volume starts in the image, by disc format (the first match wins).
VOLUME_OFFSETS = ((0x2080000, "XGD3"), (0xFD90000, "XGD2"), (0x18300000, "XGD1"),
                  (0x2090000, "XGD3"), (0, "plain XDVDFS"))


class IsoError(Exception):
    pass


def find_volume(f):
    """(offset, format name) of the XDVDFS volume in an open image, or None."""
    for off, name in VOLUME_OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(len(xc.MEDIA_MAGIC)) == xc.MEDIA_MAGIC:
            return off, name
    return None


def is_iso(path):
    """True when the file holds an XDVDFS volume at a disc offset (cheap: a few small reads)."""
    try:
        with open(path, "rb") as f:
            return find_volume(f) is not None
    except OSError:
        return False


class Iso(xc.Xdvdfs):
    BLOCK = SECTOR

    def __init__(self, path):
        try:
            self.files = [open(path, "rb")]
        except OSError as e:
            raise IsoError(f"Cannot read {path}: {e}")
        f = self.files[0]
        found = find_volume(f)
        if not found:
            f.close()
            raise IsoError("Not an Xbox 360 disc image: no game partition found at the "
                           "XGD1, XGD2 or XGD3 offset.")
        self.offset, self.format = found
        f.seek(self.offset + 32 * SECTOR + len(xc.MEDIA_MAGIC))
        self.root_block, self.root_size = struct.unpack("<II", f.read(8))
        self.size = os.fstat(f.fileno()).st_size

    def close(self):
        for f in self.files:
            f.close()

    def read_block(self, block, n=SECTOR):
        f = self.files[0]
        f.seek(self.offset + block * SECTOR)
        return f.read(n)

    def runs_for(self, block, length):
        return [[0, self.offset + block * SECTOR, length]] if length else []

    def entries(self):
        out = super().entries()
        for e in out:
            if not e.is_dir and e.runs and e.runs[0][1] + e.size > self.size:
                raise IsoError(f"The image is truncated: {e.path} runs past its end.")
        return out


def open_iso(path):
    return Iso(str(path))


def extract(iso, outdir, progress=None, cancelled=lambda: False, wanted=None):
    """Extract every file (or those under the `wanted` paths) to outdir. progress(done, total,
    name) is called before each file; cancelled() is polled between files. Returns the bytes
    written."""
    entries = iso.entries()
    if wanted:
        wanted = [p.replace("\\", "/").strip("/").lower() for p in wanted]
        entries = [e for e in entries if any(e.path.lower() == w or e.path.lower().startswith(w + "/")
                                             for w in wanted)]
    total = sum(e.size for e in entries if not e.is_dir)
    done = 0
    for e in entries:
        if cancelled():
            raise InterruptedError("cancelled")
        if progress:
            progress(done, total, e.path)
        xc.extract(iso, e, outdir)
        done += e.size
    if progress:
        progress(total, total, "")
    return total


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    path, cmd = argv[1], argv[2]
    try:
        iso = open_iso(path)
        if cmd == "info":
            print(f"format={iso.format} volume_offset=0x{iso.offset:X} root_sector={iso.root_block} "
                  f"root_size={iso.root_size} image_size={iso.size}")
            return
        if cmd == "list":
            for e in iso.entries():
                print(f"{'d' if e.is_dir else '-'} {e.size:12d} {e.path}")
            return
        if cmd == "extract" and len(argv) > 3:
            total = extract(iso, argv[3], wanted=argv[4:])
            print(f"extracted {total} bytes to {argv[3]}")
            return
    except IsoError as e:
        raise SystemExit(str(e))
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
