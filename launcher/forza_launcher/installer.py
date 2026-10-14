"""Extracting the player's game package and DLC with tools/xcontent_extract.py and
tools/install_dlc.py (imported as they are; both only read the package)."""
import contextlib
import io
import os
import shutil
from pathlib import Path

from . import TITLE_ID, paths
from .fetch import Cancelled  # noqa: F401

paths.ensure_tools_importable()
import install_dlc  # noqa: E402
import xcontent_extract  # noqa: E402
import xiso_extract  # noqa: E402

GOD = 0x7000
# Not copied from a disc image: the console system update the disc carries (tools/xiso_extract.py).
DISC_SKIP = ("$systemupdate",)


class InstallError(Exception):
    pass


def resolve_package(path: str) -> Path:
    """Accepts the package file, its <name>.data folder, or the 00007000 folder."""
    p = Path(path).expanduser()
    if p.is_dir():
        if p.name.endswith(".data"):
            sibling = p.with_name(p.name[:-5])
            if sibling.is_file():
                return sibling
        files = [f for f in sorted(p.iterdir()) if f.is_file()
                 and (p / (f.name + ".data")).is_dir()]
        if len(files) == 1:
            return files[0]
        raise InstallError("Choose the package file itself (the file next to its .data folder).")
    if not p.is_file():
        raise InstallError(f"{p} does not exist.")
    return p   # a Games on Demand package or a disc image (extract_game tells which)


def is_disc_image(path: Path) -> bool:
    return path.is_file() and xiso_extract.is_iso(path)


def inspect_package(path: Path):
    """Opens a package; returns (package, header). Errors become InstallError."""
    try:
        pkg = xcontent_extract.open_package(str(path))
    except SystemExit as e:
        raise InstallError(str(e) or "Not an Xbox 360 content package.")
    except Exception as e:  # truncated or foreign file
        raise InstallError(f"Could not read {path.name}: {e}")
    return pkg, pkg.hdr


def free_space(path: Path) -> int:
    p = path
    while not p.exists() and p != p.parent:
        p = p.parent
    return shutil.disk_usage(p).free


def extract_game(package: str, dest: Path, progress, cancelled=lambda: False):
    """Extract a Games on Demand package to dest (default.xex ends up in dest). progress(done,
    total, name) is called per file. Extracts to a sibling folder first so a failed or
    cancelled run never leaves a half game behind."""
    pkg_path = resolve_package(package)
    if is_disc_image(pkg_path):   # a disc image (ISO): the same XDVDFS tree, at the disc's offset
        try:
            pkg = xiso_extract.open_iso(pkg_path)
            entries = [e for e in pkg.entries() if e.path.split("/")[0].lower() not in DISC_SKIP]
        except xiso_extract.IsoError as e:
            raise InstallError(str(e))
    else:
        pkg, hdr = inspect_package(pkg_path)
        if f"{hdr.title_id:08X}" != TITLE_ID:
            raise InstallError(f"This package is for title {hdr.title_id:08X}, not Forza Horizon "
                               f"({TITLE_ID}).")
        if hdr.content_type != GOD:
            raise InstallError("This is not a Games on Demand package (the base game is the file "
                               f"under {TITLE_ID}/00007000). DLC goes under Install DLC.")
        try:
            entries = pkg.entries()
        except SystemExit as e:
            raise InstallError(str(e))
    if not any(e.path.lower() == "default.xex" for e in entries):
        raise InstallError("There is no default.xex here: this is not the base game. For a disc, "
                           "choose Disc 1, the one with default.xex.")
    total = sum(e.size for e in entries if not e.is_dir)
    part = dest.with_name(dest.name + ".part")
    if free_space(dest.parent) < total + (1 << 28):
        raise InstallError(f"Not enough free disk space: the game needs {total / 2**30:.1f} GB.")
    shutil.rmtree(part, ignore_errors=True)
    part.mkdir(parents=True)
    done = 0
    try:
        # default.xex first: a title update or another game is refused before the long copy
        entries = sorted(entries, key=lambda e: e.path.lower() != "default.xex")
        for n, e in enumerate(entries):
            if cancelled():
                raise Cancelled()
            progress(done, total, e.path)
            xcontent_extract.extract(pkg, e, str(part))
            done += 0 if e.is_dir else e.size
            if n == 0 and e.path.lower() == "default.xex":
                ok, msg = inspect_xex(part / "default.xex")
                if not ok:
                    raise InstallError(msg)
        if not (part / "default.xex").is_file():
            raise InstallError("The package has no default.xex: is it the base game?")
    except (Cancelled, InstallError):
        shutil.rmtree(part, ignore_errors=True)
        raise
    except SystemExit as e:
        shutil.rmtree(part, ignore_errors=True)
        raise InstallError(str(e))
    except OSError as e:
        shutil.rmtree(part, ignore_errors=True)
        raise InstallError(f"Disk error: {e}")
    finally:
        for f in getattr(pkg, "files", []):
            with contextlib.suppress(Exception):
                f.close()
    if dest.exists():
        shutil.rmtree(dest)
    os.replace(part, dest)
    progress(total, total, "")
    return total


_DLC_CACHE = {}


def describe_dlc_folder(folder: str):
    """[(file name, display name)] of the DLC packages in a folder, or raises InstallError.
    Cached per folder and modification time (each package is opened to read its name)."""
    p = Path(folder).expanduser()
    try:
        key = (str(p), p.stat().st_mtime_ns)
    except OSError:
        key = (str(p), 0)
    if key not in _DLC_CACHE:
        found = list_dlc_packages(p)
        for _f, _n, pkg in found:
            for f in getattr(pkg, "files", []):
                try:
                    f.close()
                except Exception:
                    pass
        _DLC_CACHE[key] = [(f.name, name) for f, name, _ in found]
    return _DLC_CACHE[key]


def list_dlc_packages(folder: Path):
    """The files of a DLC folder that are STFS marketplace packages: [(path, display name, size)]."""
    out = []
    root = folder
    if not root.is_dir():
        raise InstallError(f"{root} is not a folder.")
    for f in sorted(root.iterdir()):
        if not f.is_file():
            continue
        try:
            pkg = xcontent_extract.open_package(str(f))
        except (SystemExit, Exception):
            continue
        if pkg.hdr.content_type == install_dlc.MARKETPLACE:
            out.append((f, pkg.hdr.display_name, pkg))
    return out


def install_dlc_folder(folder: str, user_data: Path, progress, cancelled=lambda: False, log=print):
    """Install every DLC package of a folder. Returns the number of packages installed."""
    found = list_dlc_packages(Path(folder).expanduser())
    if not found:
        raise InstallError("No DLC packages in that folder (it is the 4D5309C9/00000002 folder "
                           "of your content).")
    total = 0
    for _f, _n, pkg in found:
        try:
            total += sum(e.size for e in pkg.entries() if not e.is_dir)
        except SystemExit:
            pass
    done = [0]
    real_extract = xcontent_extract.extract

    def counted(pkg, entry, outdir):
        if cancelled():
            raise Cancelled()
        progress(done[0], total, entry.path)
        real_extract(pkg, entry, outdir)
        if not entry.is_dir:
            done[0] += entry.size

    xcontent_extract.extract = counted
    count = 0
    try:
        for f, name, _pkg in found:
            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    install_dlc.install(str(f), str(user_data))
            except SystemExit as e:
                log(f"{f.name}: {e}")
                continue
            for line in buf.getvalue().splitlines():
                log(line)
            count += 1
    finally:
        xcontent_extract.extract = real_extract
    progress(total, total, "")
    return count


# --- checking default.xex --------------------------------------------------------------------

# The base-version executables the project is made for, by (version, entry point). Disc layouts of
# one build differ only in the header and a few data strings (docs/disc.md); add a region's here
# once its code is checked (tools/apply_title_update.py prints how many page groups match).
KNOWN_BASE = {(7, 0x82A6FBC0): "Forza Horizon base version"}
BASE_VERSION = 7
REQUIRED_FILES = ("default.xex", "XMediaFacade_default.xex", "SpeechFacade_default.xex")


def xex_version(v: int) -> str:
    return f"{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def inspect_xex(path: Path):
    """Read a default.xex's header. Returns (ok, message). The base version of Forza Horizon
    (title 4D5309C9, version 0.0.0.7, entry point 0x82A6FBC0) is what the project is made for;
    the title updates (TU4 and others) have other code and do not work."""
    import struct
    try:
        with open(path, "rb") as f:
            raw = f.read(0x4000)
    except OSError as e:
        return False, f"Cannot read {path}: {e}"
    if raw[:4] != b"XEX2":
        return False, f"{path.name} is not an Xbox 360 executable (XEX2)."
    try:
        count = struct.unpack_from(">I", raw, 0x14)[0]
        opts = {}
        for i in range(count):
            key, val = struct.unpack_from(">II", raw, 0x18 + i * 8)
            opts[key] = val
        media, version, base, title = struct.unpack_from(">IIII", raw, opts[0x40006])
        entry = opts.get(0x10100)
    except (struct.error, KeyError):
        return False, f"{path.name} has an unusual header; it is not Forza Horizon's default.xex."
    if f"{title:08X}" != TITLE_ID:
        return False, (f"This is not Forza Horizon: its title id is {title:08X}, not {TITLE_ID}. "
                       "Point to the game's Games on Demand package or disc (Disc 1).")
    found = f"version {xex_version(version)}, base {xex_version(base)}, media id {media:08X}, entry point {entry or 0:#x}"
    if (version, entry) in KNOWN_BASE and version == base:
        return True, f"{KNOWN_BASE[(version, entry)]} ({xex_version(version)}, media id {media:08X})."
    if version != base or version > BASE_VERSION:   # TU4 is 0.0.4.12 (base 0.0.0.12)
        return False, (f"This default.xex is {found}: it looks like a title update (TU4 or another). "
                       "This project needs the base version of the game, the one on the disc or in the "
                       "plain Games on Demand package, without title updates applied.")
    return False, (f"This default.xex ({found}) is not a version this project knows (0.0.0.7, entry "
                   "point 0x82a6fbc0). Another region's disc may differ: please report it with the numbers above (GitHub issues).")


def check_game_folder(folder: Path):
    """(ok, message) for an extracted game folder: the three XEX files the build needs and
    a base-version default.xex."""
    missing = [n for n in REQUIRED_FILES if not (folder / n).is_file()]
    if missing:
        return False, (f"{folder} is missing {', '.join(missing)}. Choose the folder of the whole extracted "
                       f"game, the one that holds {', '.join(REQUIRED_FILES)} (a default.xex alone is not enough).")
    return inspect_xex(folder / "default.xex")


TITLE_UPDATE = 0xB0000


def inspect_title_update(path: str):
    """(ok, message) for the player's title update package (TU4: the package of the 4000D145
    update, version 4)."""
    import tempfile
    import zipfile
    tmp = None
    try:
        p = Path(path).expanduser()
        if p.is_file() and zipfile.is_zipfile(p):   # a zip holding the package (apply_title_update reads it too)
            with zipfile.ZipFile(p) as z:
                members = [i for i in z.infolist() if not i.is_dir() and i.file_size > 0x1000]
                if len(members) != 1:
                    return False, "This zip does not hold a single title update package."
                tmp = tempfile.mkdtemp(prefix="forza-tu-")
                p = Path(z.extract(members[0], tmp))
        elif p.is_dir():
            files = [f for f in p.rglob("*") if f.is_file()]
            if len(files) != 1:
                return False, "Choose the title update package file itself."
            p = files[0]
        pkg, hdr = inspect_package(p)
    except (InstallError, OSError, zipfile.BadZipFile) as e:
        return False, str(e)
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    if f"{hdr.title_id:08X}" != TITLE_ID:
        return False, f"This package is for title {hdr.title_id:08X}, not Forza Horizon."
    if hdr.content_type != TITLE_UPDATE:
        return False, (f"This is not a title update (content type {hdr.content_type:#x}); the title update is "
                       "the package under 4D5309C9/000B0000.")
    return True, f"Title update package: {hdr.display_name or p.name}."
