#!/usr/bin/env python3
"""Apply a title update to Forza Horizon's executables: base XEX + the update's delta patch
(.xexp) -> a patched XEX that codegen reads like the original.

Usage:
  apply_title_update.py BASE_XEX TITLE_UPDATE OUT_XEX
      BASE_XEX      the disc's default.xex. The facade DLLs (XMediaFacade_default.xex,
                    SpeechFacade_default.xex) next to it are patched too when the update
                    has patches for them; they are written next to OUT_XEX.
      TITLE_UPDATE  the title update: its XContent package (content type 0xB0000, e.g.
                    4D5309C9/000B0000/tu00000001_00000000), a .zip holding it, a folder
                    holding one of those or the extracted files, or a bare .xexp.
      OUT_XEX       the patched default.xex. The update's other files (Forza Horizon's
                    TU4 has media.zip) are extracted next to it: OUT_XEX's folder is
                    what the game mounts as update: (the update_data_root setting).
  apply_title_update.py --check XEX     print the version, module flags and page-digest check

The output XEX is unencrypted, with "basic" (no) compression and the target headers of the
update, so the ReXGlue SDK, tools/xex_image.py and Ghidra's XEX loader read it as they do
the disc's. The image is checked against the update's page digests (SHA-1 chain in the
security info), so a wrong key or a decoding error never produces an XEX silently.

Standard library only (AES and LZX are in pure Python; AES uses the `cryptography`
module or the openssl command when one is there, which is much faster). Inputs are only
read. Same process as the SDK's XexModule::ApplyPatch (from Xenia).
"""
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzx_delta  # noqa: E402

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
HDR_FILE_FORMAT = 0x3FF
HDR_DELTA_PATCH = 0x5FF
HDR_EXECUTION_INFO = 0x40006
FLAG_PATCH_DELTA = 0x40
FLAG_PATCH_FULL = 0x20
FACADES = ("XMediaFacade_default.xex", "SpeechFacade_default.xex")
TITLE_UPDATE_TYPE = 0xB0000


# ---------------------------------------------------------------- AES-128 (decrypt only)

def _aes_tables():
    sbox = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    inv = [0] * 256
    for i, v in enumerate(sbox):
        inv[v] = i
    return sbox, inv


_SBOX, _INV_SBOX = _aes_tables()


def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xt(a)
        b >>= 1
    return r


_TD = []
for _k in range(4):
    _t = []
    for _x in range(256):
        s = _INV_SBOX[_x]
        col = [_mul(s, 14), _mul(s, 9), _mul(s, 13), _mul(s, 11)]
        col = col[-_k:] + col[:-_k] if _k else col
        _t.append((col[0] << 24) | (col[1] << 16) | (col[2] << 8) | col[3])
    _TD.append(_t)


def _expand_key(key):
    w = list(struct.unpack(">4I", key))
    rcon = 1
    for i in range(4, 44):
        t = w[i - 1]
        if i % 4 == 0:
            t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
            t = (_SBOX[t >> 24] << 24) | (_SBOX[(t >> 16) & 0xFF] << 16) | (_SBOX[(t >> 8) & 0xFF] << 8) | _SBOX[t & 0xFF]
            t ^= rcon << 24
            rcon = _xt(rcon)
        w.append(w[i - 4] ^ t)
    # Decryption round keys: reversed, InvMixColumns applied to the middle rounds.
    rounds = [w[4 * r:4 * r + 4] for r in range(11)][::-1]
    for r in range(1, 10):
        rounds[r] = [_TD[0][_SBOX[x >> 24]] ^ _TD[1][_SBOX[(x >> 16) & 0xFF]] ^
                     _TD[2][_SBOX[(x >> 8) & 0xFF]] ^ _TD[3][_SBOX[x & 0xFF]] for x in rounds[r]]
    return rounds


def _decrypt_block(rk, block):
    s0, s1, s2, s3 = struct.unpack(">4I", block)
    k = rk[0]
    s0 ^= k[0]; s1 ^= k[1]; s2 ^= k[2]; s3 ^= k[3]
    t0, t1, t2, t3 = _TD
    for r in range(1, 10):
        k = rk[r]
        n0 = t0[s0 >> 24] ^ t1[(s3 >> 16) & 0xFF] ^ t2[(s2 >> 8) & 0xFF] ^ t3[s1 & 0xFF] ^ k[0]
        n1 = t0[s1 >> 24] ^ t1[(s0 >> 16) & 0xFF] ^ t2[(s3 >> 8) & 0xFF] ^ t3[s2 & 0xFF] ^ k[1]
        n2 = t0[s2 >> 24] ^ t1[(s1 >> 16) & 0xFF] ^ t2[(s0 >> 8) & 0xFF] ^ t3[s3 & 0xFF] ^ k[2]
        n3 = t0[s3 >> 24] ^ t1[(s2 >> 16) & 0xFF] ^ t2[(s1 >> 8) & 0xFF] ^ t3[s0 & 0xFF] ^ k[3]
        s0, s1, s2, s3 = n0, n1, n2, n3
    k = rk[10]
    ib = _INV_SBOX
    out = []
    for a, b, c, d, kk in ((s0, s3, s2, s1, k[0]), (s1, s0, s3, s2, k[1]),
                           (s2, s1, s0, s3, k[2]), (s3, s2, s1, s0, k[3])):
        out.append(((ib[a >> 24] << 24) | (ib[(b >> 16) & 0xFF] << 16) |
                    (ib[(c >> 8) & 0xFF] << 8) | ib[d & 0xFF]) ^ kk)
    return struct.pack(">4I", *out)


def aes_decrypt(key, data, cbc):
    """AES-128 decryption, ECB or CBC with a zero IV; len(data) is a multiple of 16."""
    data = bytes(data)
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        mode = modes.CBC(bytes(16)) if cbc else modes.ECB()
        d = Cipher(algorithms.AES(key), mode).decryptor()
        return d.update(data) + d.finalize()
    except ImportError:
        pass
    openssl = shutil.which("openssl")
    if openssl:
        args = [openssl, "enc", "-d", "-aes-128-cbc" if cbc else "-aes-128-ecb", "-K", key.hex(), "-nopad"]
        if cbc:
            args += ["-iv", "00" * 16]
        # A frozen launcher (PyInstaller) points LD_LIBRARY_PATH at its own libraries, which
        # the system openssl must not load; on any failure the pure-Python AES below is used.
        env = dict(os.environ)
        if "LD_LIBRARY_PATH_ORIG" in env:
            env["LD_LIBRARY_PATH"] = env["LD_LIBRARY_PATH_ORIG"]
        else:
            env.pop("LD_LIBRARY_PATH", None)
        try:
            out = subprocess.run(args, input=data, capture_output=True, check=True, env=env).stdout
            if len(out) == len(data):
                return out
        except (OSError, subprocess.CalledProcessError):
            pass
    rk = _expand_key(key)
    out = bytearray()
    prev = bytes(16)
    for i in range(0, len(data), 16):
        block = data[i:i + 16]
        plain = _decrypt_block(rk, block)
        if cbc:
            plain = bytes(a ^ b for a, b in zip(plain, prev))
            prev = block
        out += plain
    return bytes(out)


# ---------------------------------------------------------------- XEX2 structures

class Xex:
    def __init__(self, raw, name="xex"):
        if raw[:4] != b"XEX2":
            raise SystemExit(f"{name}: not an XEX2 file")
        self.raw = raw
        self.name = name
        _, self.flags, self.header_size, _, self.sec, count = struct.unpack_from(">4sIIIII", raw, 0)
        self.opts = {}
        for i in range(count):
            key, value = struct.unpack_from(">II", raw, 0x18 + i * 8)
            self.opts[key] = value
        if HDR_FILE_FORMAT not in self.opts:
            raise SystemExit(f"{name}: no file format header")
        self.ffi = self.opts[HDR_FILE_FORMAT]
        self.ffi_size, self.encryption, self.compression = struct.unpack_from(">IHH", raw, self.ffi)
        self.image_size, = struct.unpack_from(">I", raw, self.sec + 4)
        self.load_address, = struct.unpack_from(">I", raw, self.sec + 0x110)

    @property
    def aes_key(self):
        return self.raw[self.sec + 0x150:self.sec + 0x160]

    @property
    def rsa_signature(self):
        return self.raw[self.sec + 8:self.sec + 0x108]

    def session_key(self):
        return aes_decrypt(RETAIL_KEY, self.aes_key, cbc=False)

    def version(self):
        if HDR_EXECUTION_INFO not in self.opts:
            return None
        v, = struct.unpack_from(">I", self.raw, self.opts[HDR_EXECUTION_INFO] + 4)
        return fmt_version(v)

    def payload(self):
        return self.raw[self.header_size:]

    def image(self):
        """The loaded image (decrypted, decompressed), padded to the page descriptors' size."""
        data = self.payload()
        if self.encryption == 1:
            data = aes_decrypt(self.session_key(), data[:len(data) // 16 * 16], cbc=True)
        elif self.encryption != 0:
            raise SystemExit(f"{self.name}: unknown encryption type {self.encryption}")
        if self.compression == 1:
            image = bytearray()
            pos = 0
            for off in range(self.ffi + 8, self.ffi + self.ffi_size, 8):
                data_size, zero_size = struct.unpack_from(">II", self.raw, off)
                image += data[pos:pos + data_size]
                image += bytes(zero_size)
                pos += data_size
        elif self.compression == 0:
            image = bytearray(data)
        else:
            raise SystemExit(f"{self.name}: LZX-compressed images are not supported")
        size = self.pages_size()
        if len(image) < size:
            image += bytes(size - len(image))
        return image

    def page_descriptors(self):
        count, = struct.unpack_from(">I", self.raw, self.sec + 0x180)
        return [struct.unpack_from(">I20s", self.raw, self.sec + 0x184 + i * 24) for i in range(count)]

    def page_size(self):
        flags, = struct.unpack_from(">I", self.raw, self.sec + 0x10C)
        return 0x1000 if flags & 0x10000000 else 0x10000

    def pages_size(self):
        return sum((v >> 4) for v, _ in self.page_descriptors()) * self.page_size()


def fmt_version(v):
    return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def check_pages(xex, image):
    """XEX2 page digests: SHA-1(pages of descriptor i + descriptor i) is stored in
    descriptor i-1 (for i = 0: the security info's section digest). Returns (ok, i)."""
    descs = xex.page_descriptors()
    ps = xex.page_size()
    expected = xex.raw[xex.sec + 0x114:xex.sec + 0x128]
    off = 0
    for i, (v, digest) in enumerate(descs):
        size = (v >> 4) * ps
        h = hashlib.sha1(image[off:off + size])
        h.update(struct.pack(">I", v) + digest)
        if h.digest() != expected:
            return False, i
        expected = digest
        off += size
    return True, None


# ---------------------------------------------------------------- patching

def packed_header(raw):
    """The same XEX headers laid out the way Microsoft's tools write them: the security info
    right after the optional header table, then each header's data back to back in their
    order, the import libraries left in place. Some copies of the game have the security
    info and the data moved (0x80 bytes of zeros after the table, 16-byte alignment), with
    the same content; a delta patch made for the original layout needs this one."""
    sec, count = struct.unpack_from(">II", raw, 0x10)
    sec_size, = struct.unpack_from(">I", raw, sec)
    out = bytearray(len(raw))
    table_end = 0x18 + count * 8
    out[:table_end] = raw[:table_end]
    struct.pack_into(">I", out, 0x10, table_end)
    pos = table_end
    out[pos:pos + sec_size] = raw[sec:sec + sec_size]
    pos += sec_size
    entries = []
    for i in range(count):
        key, value = struct.unpack_from(">II", raw, 0x18 + i * 8)
        if key & 0xFF in (0, 1):
            continue  # value held in the table
        size = struct.unpack_from(">I", raw, value)[0] if key & 0xFF == 0xFF else (key & 0xFF) * 4
        entries.append((value, i, key, size))
    for value, i, key, size in sorted(entries):
        if key == 0x103FF:  # import libraries: kept where they are
            out[value:value + size] = raw[value:value + size]
            continue
        out[pos:pos + size] = raw[value:value + size]
        struct.pack_into(">I", out, 0x18 + i * 8 + 4, pos)
        pos += size
    return bytes(out)


class PatchFailed(Exception):
    pass


def apply_patch(base, patch):
    """Return (header bytes, image, Xex of the new headers, unverified ranges) of the base
    XEX with the delta patch applied, checked against the patch's block hashes and the new
    page digests."""
    if not patch.flags & FLAG_PATCH_DELTA:
        raise SystemExit(f"{patch.name}: not a delta patch (module flags {patch.flags:#x})")
    if HDR_DELTA_PATCH not in patch.opts:
        raise SystemExit(f"{patch.name}: no delta patch descriptor")
    if patch.compression not in (2, 3):  # normal (LZX) or delta
        raise SystemExit(f"{patch.name}: unexpected compression type {patch.compression}")
    d = patch.opts[HDR_DELTA_PATCH]
    source_ver, = struct.unpack_from(">I", patch.raw, d + 8)
    # A title update is made for one disc's XEX (the hash of its signature). Copies of the
    # same executable can differ in their headers only (signature, layout), so a mismatch is
    # only reported: the result has to pass the update's own block hashes and page digests.
    if hashlib.sha1(base.rsa_signature).digest() != patch.raw[d + 0xC:d + 0x20]:
        print(f"note: {patch.name} was made for another copy of {base.name} (version "
              f"{fmt_version(source_ver)}; this one is {base.version()}); trying it")
    base_header = base.raw[:base.header_size]
    image = base.image()
    errors = []
    for label, header in (("as is", base_header), ("repacked", packed_header(base_header))):
        try:
            return _apply(base, patch, header, bytearray(image))
        except (PatchFailed, lzx_delta.LzxError, struct.error) as e:
            errors.append(f"{label}: {e}")
    raise SystemExit(f"{patch.name} does not apply to {base.name}: " + "; ".join(errors))


def _apply(base, patch, base_header, image):
    raw = patch.raw
    d = patch.opts[HDR_DELTA_PATCH]
    image_key_source = raw[d + 0x20:d + 0x30]
    (target_headers, hdr_src_off, hdr_src_size, hdr_tgt_off,
     img_src_off, img_src_size, img_tgt_off) = struct.unpack_from(">7I", raw, d + 0x30)
    window, = struct.unpack_from(">I", raw, patch.ffi + 8)

    # Headers: the delta applies to the base header bytes.
    header = bytearray(base_header)
    if not target_headers:
        target_headers = hdr_tgt_off + hdr_src_size
    if target_headers > len(header):
        header += bytes(target_headers - len(header))
    if hdr_src_off:
        header[hdr_tgt_off:hdr_tgt_off + hdr_src_size] = header[hdr_src_off:hdr_src_off + hdr_src_size]
    if target_headers < len(header):
        header[target_headers:] = bytes(len(header) - target_headers)
    # The header records run to the end of the descriptor (its size field). The SDK (from
    # Xenia) bounds them by the first record's length, which is the same when that record
    # is the only one, as in default.xexp; the facades' patches have several.
    desc_size, = struct.unpack_from(">I", raw, d)
    lzx_delta.apply_delta_records(raw[d + 0x4C:d + desc_size], window, header)
    del header[target_headers:]
    if header[:4] != b"XEX2":
        raise PatchFailed("patched headers are not XEX2")
    new = Xex(bytes(header), "patched headers")
    new_size = new.pages_size()
    if new.image_size != new_size:
        raise PatchFailed("patched headers are inconsistent")

    # Keys: the patched header holds the new image key; the patch's key is encrypted with it.
    new_session = new.session_key()
    patch_session = aes_decrypt(new_session, patch.aes_key, cbc=False)
    if aes_decrypt(new_session, image_key_source, cbc=False) != base.session_key():
        print("note: the image key check does not match this copy; continuing")

    # Image.
    orig_size = len(image)
    if new_size > len(image):
        image += bytes(new_size - len(image))
    if img_src_off:
        image[img_tgt_off:img_tgt_off + img_src_size] = image[img_src_off:img_src_off + img_src_size]
    img_target = img_tgt_off + img_src_size
    if img_target < orig_size:
        image[img_target:orig_size] = bytes(orig_size - img_target)

    data = patch.payload()
    if patch.encryption == 1:
        data = aes_decrypt(patch_session, data[:len(data) // 16 * 16], cbc=True)
    block_size, block_hash = struct.unpack_from(">I20s", raw, patch.ffi + 12)
    p = 0
    n = 0
    while block_size:
        block = data[p:p + block_size]
        if len(block) != block_size or hashlib.sha1(block).digest() != block_hash:
            raise PatchFailed(f"patch block {n} hash mismatch (wrong key)")
        next_size, next_hash = struct.unpack_from(">I20s", block, 0)
        lzx_delta.apply_delta_records(block[24:], window, image)
        p += block_size
        block_size, block_hash = next_size, next_hash
        n += 1
    del image[new_size:]
    unverified = repair_digests(new, header, image)
    if len(unverified) > max(4, len(new.page_descriptors()) // 50):
        raise PatchFailed(f"{len(unverified)} page groups fail their digests (wrong base?)")
    new = Xex(bytes(header), "patched headers")
    code = [(a, e) for a, e in code_ranges(image) if any(a < u_end and u < e for u, u_end in unverified)]
    if code:
        raise PatchFailed("code pages fail their digests: " + ", ".join(f"{u:#x}-{e:#x}" for u, e in unverified))
    for u, e in unverified:
        print(f"warning: {u + new.load_address:#010x}-{e + new.load_address:#010x} (data) does not match the "
              f"update's digest: this copy's base differs there; kept as patched")
    # The execution info keeps the base's version where the delta copied it; name the update's.
    if HDR_EXECUTION_INFO in new.opts:
        target_ver, = struct.unpack_from(">I", raw, d + 4)
        struct.pack_into(">I", header, new.opts[HDR_EXECUTION_INFO] + 4, target_ver)
    return bytes(header), image, Xex(bytes(header), "patched headers"), unverified


def code_ranges(image):
    """Image offsets of the PE sections marked executable."""
    pe, = struct.unpack_from("<I", image, 0x3C)
    count, opt_size = struct.unpack_from("<H", image, pe + 6)[0], struct.unpack_from("<H", image, pe + 20)[0]
    table = pe + 24 + opt_size
    out = []
    for i in range(count):
        vsize, vaddr = struct.unpack_from("<II", image, table + i * 40 + 8)
        flags, = struct.unpack_from("<I", image, table + i * 40 + 36)
        if flags & 0x20000000:
            out.append((vaddr, vaddr + vsize))
    return out


def repair_digests(xex, header, image):
    """Check the page digest chain link by link. Where the delta copied digest bytes from
    the source headers (a different copy has other digests), a digest is wrong although
    the pages are right: a digest whose two links both fail is recomputed from the next
    descriptor (from the end, so runs of wrong digests are rebuilt). Writes repaired digests
    into `header`; returns the image ranges that still fail (pages that differ)."""
    descs = [list(x) for x in xex.page_descriptors()]
    ps = xex.page_size()
    offs = []
    off = 0
    for v, _ in descs:
        offs.append(off)
        off += (v >> 4) * ps
    top = xex.raw[xex.sec + 0x114:xex.sec + 0x128]

    def link(i):
        v, dg = descs[i]
        expected = descs[i - 1][1] if i else top
        h = hashlib.sha1(image[offs[i]:offs[i] + (v >> 4) * ps])
        h.update(struct.pack(">I", v) + dg)
        return h.digest() == expected

    n = len(descs)
    for i in range(n - 2, -1, -1):
        if not link(i + 1) and not link(i):
            v, dg = descs[i + 1]
            h = hashlib.sha1(image[offs[i + 1]:offs[i + 1] + (v >> 4) * ps])
            h.update(struct.pack(">I", v) + dg)
            descs[i][1] = h.digest()
    bad = []
    for i in range(n):
        if not link(i):
            bad.append((offs[i], offs[i] + (descs[i][0] >> 4) * ps))
    for i, (v, dg) in enumerate(descs):
        struct.pack_into(">I20s", header, xex.sec + 0x184 + i * 24, v, dg)
    return bad


def write_xex(path, header, image):
    """An XEX with the given headers whose image is stored plain (no encryption, one
    "basic" block): what the SDK and tools/xex_image.py load directly."""
    hdr = bytearray(header)
    x = Xex(bytes(hdr), "output")
    if x.ffi_size < 16:
        raise SystemExit("file format header too small to rewrite")
    # encryption none, basic compression, one data block of the whole image
    struct.pack_into(">IHHII", hdr, x.ffi, 16, 0, 1, len(image), 0)
    hdr[x.ffi + 16:x.ffi + x.ffi_size] = bytes(x.ffi_size - 16)
    header_size = (len(hdr) + 0xFFF) & ~0xFFF
    hdr += bytes(header_size - len(hdr))
    struct.pack_into(">I", hdr, 8, header_size)
    # A patch's flags describe the patch; the result is a plain module like the base.
    flags, = struct.unpack_from(">I", hdr, 4)
    struct.pack_into(">I", hdr, 4, flags & ~(FLAG_PATCH_DELTA | FLAG_PATCH_FULL | 0x10))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(hdr)
        f.write(image)
    os.replace(tmp, path)


# ---------------------------------------------------------------- title update package

def find_update_files(src, workdir):
    """Return a folder holding the title update's files (default.xexp, ...)."""
    import xcontent_extract as xc
    if os.path.isdir(src):
        if os.path.isfile(os.path.join(src, "default.xexp")):
            return src
        for root, _, files in os.walk(src):
            for f in files:
                p = os.path.join(root, f)
                if is_update_package(p):
                    return find_update_files(p, workdir)
        raise SystemExit(f"{src}: no title update package or default.xexp in this folder")
    if src.lower().endswith(".xexp"):
        return os.path.dirname(os.path.abspath(src))
    if zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            for info in z.infolist():
                if info.is_dir() or info.file_size < 0x971A:
                    continue
                with z.open(info) as f:
                    head = f.read(0x350)
                if head[:4] in (b"CON ", b"LIVE", b"PIRS") and struct.unpack_from(">I", head, 0x344)[0] == TITLE_UPDATE_TYPE:
                    out = os.path.join(workdir, "package")
                    with z.open(info) as f, open(out, "wb") as o:
                        shutil.copyfileobj(f, o)
                    return find_update_files(out, workdir)
        raise SystemExit(f"{src}: no title update package in this zip")
    if not is_update_package(src):
        raise SystemExit(f"{src}: not a title update package (XContent type 0xB0000)")
    pkg = xc.open_package(src)
    out = os.path.join(workdir, "files")
    os.makedirs(out, exist_ok=True)
    for e in pkg.entries():
        if not e.is_dir:
            xc.extract(pkg, e, out)
    return out


def is_update_package(path):
    try:
        with open(path, "rb") as f:
            head = f.read(0x350)
    except OSError:
        return False
    return (len(head) == 0x350 and head[:4] in (b"CON ", b"LIVE", b"PIRS")
            and struct.unpack_from(">I", head, 0x344)[0] == TITLE_UPDATE_TYPE)


def patch_file(base_path, xexp_path, out_path):
    base = Xex(open(base_path, "rb").read(), os.path.basename(base_path))
    patch = Xex(open(xexp_path, "rb").read(), os.path.basename(xexp_path))
    header, image, new, unverified = apply_patch(base, patch)
    write_xex(out_path, header, image)
    pages = len(new.page_descriptors())
    print(f"{out_path}: {base.name} {base.version()} -> {new.version()}, image {len(image):#x} bytes, "
          f"{pages - len(unverified)} of {pages} page groups match the update's digests")


def main(argv):
    if len(argv) == 3 and argv[1] == "--check":
        x = Xex(open(argv[2], "rb").read(), argv[2])
        print(f"version {x.version()} flags {x.flags:#x} encryption {x.encryption} compression {x.compression}")
        if x.flags & FLAG_PATCH_DELTA:
            return 0
        ok, bad = check_pages(x, x.image())
        print("page digests OK" if ok else f"page digest mismatch at descriptor {bad}")
        return 0 if ok else 1
    if len(argv) != 4:
        print(__doc__)
        return 2
    base_xex, update, out_xex = argv[1:]
    out_dir = os.path.dirname(os.path.abspath(out_xex))
    os.makedirs(out_dir, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="forza-tu-") as work:
        files = find_update_files(update, work)
        if not os.path.isfile(os.path.join(files, "default.xexp")):
            raise SystemExit("the title update has no default.xexp")
        patch_file(base_xex, os.path.join(files, "default.xexp"), out_xex)
        base_dir = os.path.dirname(os.path.abspath(base_xex))
        for facade in FACADES:
            xexp = os.path.join(files, facade + "p")
            if os.path.isfile(xexp) and os.path.isfile(os.path.join(base_dir, facade)):
                patch_file(os.path.join(base_dir, facade), xexp, os.path.join(out_dir, facade))
        # Everything else the update carries (TU4: media.zip) goes next to the patched
        # executable: that folder is mounted as update:.
        upd = out_dir
        for root, _, names in os.walk(files):
            for n in names:
                if n.lower().endswith(".xexp"):
                    continue
                src = os.path.join(root, n)
                dst = os.path.join(upd, os.path.relpath(src, files))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                print(f"{dst}: from the title update")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
