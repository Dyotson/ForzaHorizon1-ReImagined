"""Is this default.xex the code the project is made for?

The game exists in several printings (retail discs, Games on Demand packages): the same build
with other version numbers and media ids, so the header says nothing about the code. This
compares the loaded image of a player's executable, section by section, with fingerprints of the
supported one (xex_reference.py: SHA-1 prefixes of 64 KB pieces of every section, no game bytes).
Reuses tools/apply_title_update.py for the decryption (pure Python when openssl and the
`cryptography` module are not there, as on Windows: a few seconds to ~20 s for the 23 MB image)."""
import hashlib
import json
import struct
from pathlib import Path

from . import paths

paths.ensure_tools_importable()
import apply_title_update as atu  # noqa: E402

from . import xex_reference  # noqa: E402

CHUNK = 0x10000
HASH_CHARS = 20
FLAG_CODE = 0x20000000


class Unreadable(Exception):
    """A file that is not an executable this tool can decode (message for the player)."""


def header_info(raw: bytes):
    """(media id, version, base version, title id, entry point or 0, module flags) from the
    XEX2 header, or None when it is not one."""
    if raw[:4] != b"XEX2":
        return None
    try:
        flags, = struct.unpack_from(">I", raw, 4)
        count, = struct.unpack_from(">I", raw, 0x14)
        opts = {}
        for i in range(count):
            key, val = struct.unpack_from(">II", raw, 0x18 + i * 8)
            opts[key] = val
        media, version, base, title = struct.unpack_from(">IIII", raw, opts[0x40006])
    except (struct.error, KeyError):
        return None
    return media, version, base, title, opts.get(0x10100, 0), flags


def _sections(image):
    pe, = struct.unpack_from("<I", image, 0x3C)
    count, = struct.unpack_from("<H", image, pe + 6)
    opt_size, = struct.unpack_from("<H", image, pe + 20)
    table = pe + 24 + opt_size
    out = []
    for i in range(count):
        o = table + i * 40
        name = bytes(image[o:o + 8]).rstrip(b"\0").decode("latin-1")
        vsize, va = struct.unpack_from("<II", image, o + 8)
        flags, = struct.unpack_from("<I", image, o + 36)
        out.append((name, va, vsize, flags))
    return out


def chunk_hashes(image, va, size):
    """SHA-1 prefixes of [va, va+size) cut at multiples of 64 KB."""
    out = []
    pos, end = va, va + size
    while pos < end:
        nxt = min(end, (pos // CHUNK + 1) * CHUNK)
        out.append(hashlib.sha1(image[pos:nxt]).hexdigest()[:HASH_CHARS])
        pos = nxt
    return out


def fingerprint(raw: bytes, name: str = "xex"):
    """The comparable description of an executable: {'image', 'entry', 'sections': [[name, va,
    size, flags, [hashes]]], 'intact'}. 'intact' tells whether the image passes the page digests
    of its own header (a damaged copy does not). Raises Unreadable."""
    try:
        x = atu.Xex(raw, name)
        if x.compression not in (0, 1) or x.encryption not in (0, 1):
            raise Unreadable(f"{name} uses a compression this launcher cannot read.")
        image = x.image()
        intact, _ = atu.check_pages(x, image)
        secs = [[n, va, vs, fl, chunk_hashes(image, va, vs)] for n, va, vs, fl in _sections(image)]
    except SystemExit as e:
        raise Unreadable(str(e))
    except (struct.error, IndexError, ValueError) as e:
        raise Unreadable(f"{name} could not be decoded ({e}).")
    info = header_info(raw)
    return {"image": len(image), "entry": info[4] if info else 0, "sections": secs, "intact": intact}


def _range(va, k, size, load):
    a = va if k == 0 else (va // CHUNK + k) * CHUNK
    b = min(va + size, (va // CHUNK + k + 1) * CHUNK)
    return f"{load + a:#010x}-{load + b - 1:#010x}"


def differences(fp, ref, load=0x82000000):
    """List of strings naming what differs between a fingerprint and the reference; empty = the
    same code and data. Names the section and the address ranges of the pieces that differ."""
    out = []
    if fp["image"] != ref["image"]:
        out.append(f"image size {fp['image']:#x} instead of {ref['image']:#x}")
    if fp["entry"] != ref["entry"]:
        out.append(f"entry point {fp['entry']:#x} instead of {ref['entry']:#x}")
    have = {s[0]: s for s in fp["sections"]}
    want = {s[0]: s for s in ref["sections"]}
    for n in sorted(set(have) | set(want), key=lambda n: (want.get(n) or have[n])[1]):
        if n not in have or n not in want:
            out.append(f"section {n} is {'missing' if n not in have else 'extra'}")
            continue
        _, va, size, flags, hs = have[n]
        _, rva, rsize, rflags, rhs = want[n]
        if (va, size, flags) != (rva, rsize, rflags):
            out.append(f"section {n} has another layout")
            continue
        bad = [k for k, (a, b) in enumerate(zip(hs, rhs)) if a != b]
        if bad:
            kind = "code" if flags & FLAG_CODE else "data"
            shown = ", ".join(_range(va, k, size, load) for k in bad[:3])
            out.append(f"section {n} ({kind}) at {shown}" + (f" and {len(bad) - 3} more" if len(bad) > 3 else ""))
    return out


# --- verified results, kept so the window's frequent checks do not decode the image again ----------

_MEMORY = {}


def _cache_file() -> Path:
    return paths.config_dir() / "xex_checks.json"


def _key(path: Path, head: bytes) -> str:
    st = path.stat()
    return f"{path.name}|{st.st_size}|{st.st_mtime_ns}|{hashlib.sha1(head).hexdigest()[:12]}|{xex_reference.REFERENCE_ID}"


def _load_disk():
    try:
        return json.loads(_cache_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _store_disk(key, value):
    try:
        data = _load_disk()
        data.pop(key, None)
        data[key] = value
        for k in list(data)[:-30]:   # the 30 newest
            del data[k]
        f = _cache_file()
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(json.dumps(data), encoding="utf-8")
    except OSError:
        pass


def cached(path: Path):
    """The remembered verdict for this file: (same_code, [differences], intact) or None."""
    try:
        with open(path, "rb") as f:
            key = _key(path, f.read(0x4000))
    except OSError:
        return None
    if key in _MEMORY:
        return _MEMORY[key]
    v = _load_disk().get(key)
    if v is not None:
        _MEMORY[key] = (v[0], v[1], v[2])
        return _MEMORY[key]
    return None


def check_code(path: Path, module: str):
    """Compare the executable at `path` with the reference of `module` ('default.xex' ...).
    Returns (same_code, [what differs], intact). Remembered per file. Raises Unreadable/OSError."""
    got = cached(path)
    if got is not None:
        return got
    ref = xex_reference.MODULES[module]
    raw = path.read_bytes()
    fp = fingerprint(raw, path.name)
    result = (not (diffs := differences(fp, ref)), diffs, fp["intact"])
    key = _key(path, raw[:0x4000])
    _MEMORY[key] = result
    _store_disk(key, list(result))
    return result
