#!/usr/bin/env python3
"""Decode FH1 car/design thumbnails: xdc_decode.py INPUT.xdc OUTPUT.png.

Read dimensions from the texture descriptor (garage: 768x288; storefront: 288x160).
The decoder and PNG writer use the standard library and can be imported by the launcher.
"""
import struct
import sys
import zlib

MAX_TEXTURE_BYTES = 32 * 1024**2
MAX_PIXELS = 4 * 1024**2
BPB_LOG2 = 2


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


def decode(data):
    """Return (width, height, RGBA bytes); reject unsupported or malformed textures."""
    if not 20 <= len(data) <= MAX_TEXTURE_BYTES:
        raise ValueError("Invalid thumbnail file size.")
    magic, version, usize, csize, repeated_size = struct.unpack_from(">4s4I", data)
    if magic != b"cxds" or version != 2 or csize != len(data)-20:
        raise ValueError("Unsupported or truncated XDC thumbnail.")
    if not 52 < usize <= MAX_TEXTURE_BYTES or repeated_size != usize:
        raise ValueError("Invalid thumbnail texture size.")
    inflater = zlib.decompressobj(-15)
    try:
        body = inflater.decompress(data[20:], usize+1)
    except zlib.error as exc:
        raise ValueError("Damaged thumbnail compression.") from exc
    if (len(body) != usize or not inflater.eof or inflater.unused_data
            or inflater.unconsumed_tail):
        raise ValueError("Thumbnail decompression does not match its declared size.")
    format_word, dimensions = struct.unpack_from(">II", body, 0x20)
    if format_word & 0x3F != 6 or not format_word & 0x80:
        raise ValueError("Only tiled 8_8_8_8 thumbnails are supported.")
    width, height = (dimensions & 0x1FFF)+1, ((dimensions >> 13) & 0x1FFF)+1
    if width * height > MAX_PIXELS:
        raise ValueError("Thumbnail dimensions exceed the pixel limit.")
    pitch = (width+31) & ~31
    pixels = body[52:]
    out = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            offset = tiled_offset(x, y, pitch)
            if offset+4 > len(pixels):
                raise ValueError("Truncated tiled thumbnail pixels.")
            _alpha, red, green, blue = pixels[offset:offset+4]
            at = (y*width+x)*4
            out[at:at+4] = bytes((red, green, blue, 255))
    return width, height, bytes(out)


def png_bytes(width, height, rgba):
    """Encode decoded RGBA pixels without a Pillow dependency."""
    if width <= 0 or height <= 0 or width*height > MAX_PIXELS or len(rgba) != width*height*4:
        raise ValueError("Invalid PNG dimensions or pixel buffer.")

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind+data)))

    stride = width*4
    rows = b"".join(b"\0"+rgba[start:start+stride] for start in range(0, len(rgba), stride))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    from pathlib import Path
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    source, destination = map(Path, sys.argv[1:])
    if source.resolve() == destination.resolve():
        raise SystemExit("Choose an output file separate from the source thumbnail.")
    with source.open("rb") as reader:
        data = reader.read(MAX_TEXTURE_BYTES+1)
    width, height, rgba = decode(data)
    with destination.open("xb") as writer:
        writer.write(png_bytes(width, height, rgba))
    print(f"{source}: {width}x{height}, {len(data)} bytes -> {destination}")


if __name__ == "__main__":
    main()
