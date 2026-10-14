#!/usr/bin/env python3
"""xdc_decode.py FILE.xdc OUT.png: decode a car thumbnail saved in the profile
(ForzaProfile/Thumbnails/Thumbnail_N.xdc; docs/framerate.md).

Layout: "cxds", version 2, uncompressed size, compressed size (big endian), then a raw
deflate stream of a D3D texture: a 0x34-byte header and the pixels, 768x288 k_8_8_8_8,
Xenos-tiled. Prints the compressed size and the pixels' entropy hint (distinct 8-bit values
of a sample): a correct car render compresses to about 20-120 KB, noise does not.
"""
import struct
import sys
import zlib

from PIL import Image

W, H, BPB_LOG2 = 768, 288, 2


def tiled_offset(x, y, width):
    macro = ((y >> 5) * (width >> 5)) << (BPB_LOG2 + 7)
    micro = ((y & 6) << 2) << BPB_LOG2
    base = (macro + ((micro & ~0xF) << 1) + (micro & 0xF)
            + ((y & 8) << (3 + BPB_LOG2)) + ((y & 1) << 4))
    macro = (x >> 5) << (BPB_LOG2 + 7)
    micro = (x & 7) << BPB_LOG2
    off = base + macro + ((micro & ~0xF) << 1) + (micro & 0xF)
    return (((off & ~0x1FF) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F)
            + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = open(src, "rb").read()
    magic, ver, usize, csize = struct.unpack(">4sIII", raw[:16])
    assert magic == b"cxds", magic
    data = zlib.decompress(raw[20:], -15)
    pix = data[0x34:]
    out = bytearray(W * H * 4)
    for y in range(H):
        for x in range(W):
            o = tiled_offset(x, y, W)
            a, b, c, d = pix[o:o + 4]  # big-endian 8_8_8_8: A R G B bytes
            i = (y * W + x) * 4
            out[i:i + 4] = bytes((b, c, d, 255))
    Image.frombytes("RGBA", (W, H), bytes(out)).save(dst)
    print(f"{src}: version {ver}, {len(raw)} bytes, texture {len(data)} bytes -> {dst}")


main()
