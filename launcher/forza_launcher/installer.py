"""Extracting the player's game package and DLC with tools/xcontent_extract.py and
tools/install_dlc.py (imported as they are; both only read the package)."""
import contextlib
import io
import os
import re
import shutil
from pathlib import Path

from . import TITLE_ID, paths
from .fetch import Cancelled  # noqa: F401

paths.ensure_tools_importable()
import install_dlc  # noqa: E402
import xcontent_extract  # noqa: E402
import xiso_extract  # noqa: E402

from . import xex_reference, xexcheck  # noqa: E402

GOD = 0x7000
# Not copied from a disc image: the console system update the disc carries (tools/xiso_extract.py).
DISC_SKIP = ("$systemupdate",)


class InstallError(Exception):
    pass


def resolve_package(path: str) -> Path:
    """Accepts the package file, its <name>.data folder or one of the DataNNNN files in it (what
    some file pickers end up on), or the 00007000 folder."""
    p = Path(path).expanduser()
    if p.is_file() and p.parent.name.endswith(".data") and re.fullmatch(r"Data\d+", p.name, re.I):
        p = p.parent   # a part of the package: the package is the file next to this folder
    if p.is_dir():
        if p.name.endswith(".data"):
            sibling = p.with_name(p.name[:-5])
            if sibling.is_file():
                return sibling
            raise InstallError(f"Choose the package file itself: the file named {sibling.name}, next to the "
                               f"{p.name} folder (it is missing here).")
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


def extract_game(package: str, dest: Path, progress, cancelled=lambda: False, note=lambda text: None):
    """Extract a Games on Demand package to dest (default.xex ends up in dest). progress(done,
    total, name) is called per file. Extracts to a sibling folder first so a failed or
    cancelled run never leaves a half game behind. note(text) gets the verdict on the
    executable (a different printing of the game is accepted and said so)."""
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
            if n == 0 and e.path.lower() == "default.xex":
                progress(done, total, "checking default.xex (can take a few seconds)")
            done += 0 if e.is_dir else e.size
            if n == 0 and e.path.lower() == "default.xex":
                ok, msg = inspect_xex(part / "default.xex")
                if not ok:
                    raise InstallError(msg)
                note(msg)
        if not (part / "default.xex").is_file():
            raise InstallError("The package has no default.xex: is it the base game?")
        ok, msg = check_facades(part)   # the DLLs of another printing are compared with the supported ones
        if not ok:
            raise InstallError(msg)
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

_NO_DLC = ("No DLC found in that folder. Pick the folder of your DLC packages, or a Xenia content "
           "folder (or its 0000000000000000, 4D5309C9 or 00000002 folder).")


def describe_dlc_folder(folder: str):
    """([(name, display name)] of the DLC in a folder, [(name, reason)] of what was skipped).
    The DLC is STFS packages (files) and/or extracted package folders (a Xenia content folder or
    one of its subfolders). Raises InstallError. Cached per folder and modification time (each
    package is opened to read its name)."""
    p = Path(folder).expanduser()
    try:
        key = (str(p), p.stat().st_mtime_ns)
    except OSError:
        key = (str(p), 0)
    if key not in _DLC_CACHE:
        packages, dirs, skipped = list_dlc(p)
        for _f, _n, pkg in packages:
            for f in getattr(pkg, "files", []):
                try:
                    f.close()
                except Exception:
                    pass
        _DLC_CACHE[key] = ([(f.name, name) for f, name, _ in packages] + [(n, disp) for _p, n, disp, _h in dirs],
                           skipped)
    return _DLC_CACHE[key]


def list_dlc(folder: Path):
    """What a DLC folder holds: ([(path, display name, package)] of the STFS marketplace packages,
    [(path, name, display name, header path)] of the extracted package folders, [(name, reason)]
    skipped folders)."""
    if not folder.is_dir():
        raise InstallError(f"{folder} is not a folder.")
    try:
        _root, dirs, files, skipped = install_dlc.scan_folder(str(folder))
    except OSError as e:
        raise InstallError(f"Cannot read {folder}: {e}")
    out = []
    for f in files:
        try:
            pkg = xcontent_extract.open_package(f)
        except (SystemExit, Exception):
            continue
        if pkg.hdr.content_type == install_dlc.MARKETPLACE:
            out.append((Path(f), pkg.hdr.display_name, pkg))
    return out, dirs, skipped


def list_dlc_packages(folder: Path):
    """The files of a DLC folder that are STFS marketplace packages: [(path, display name, size)]."""
    return list_dlc(folder)[0]


def install_dlc_folder(folder: str, user_data: Path, progress, cancelled=lambda: False, log=print):
    """Install every DLC package of a folder (package files and extracted package folders).
    Returns the number of packages installed."""
    found, dirs, skipped = list_dlc(Path(folder).expanduser())
    for n, why in skipped:
        log(f"skipping {n}: {why}")
    if not found and not dirs:
        raise InstallError(_NO_DLC)
    total = 0
    for _f, _n, pkg in found:
        try:
            total += sum(e.size for e in pkg.entries() if not e.is_dir)
        except SystemExit:
            pass
    for d, *_r in dirs:
        total += install_dlc.folder_size(d)
    done = [0]
    real_extract = xcontent_extract.extract

    def counted(pkg, entry, outdir):
        if cancelled():
            raise Cancelled()
        progress(done[0], total, entry.path)
        bad = real_extract(pkg, entry, outdir)
        if not entry.is_dir:
            done[0] += entry.size
        return bad

    def copy(src, dst):
        if cancelled():
            raise Cancelled()
        progress(done[0], total, os.path.basename(src))
        shutil.copy2(src, dst)
        done[0] += os.path.getsize(dst)

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
        for d, name, disp, header in dirs:
            try:
                install_dlc.install_folder(d, name, header, str(user_data), copy=copy, display_name=disp)
            except OSError as e:
                log(f"{name}: {e}")
                continue
            log(f"{disp}: {name}" + ("" if header else " (header generated)"))
            count += 1
    finally:
        xcontent_extract.extract = real_extract
    if not count:
        raise InstallError("None of the DLC in that folder could be installed (see the log).")
    progress(total, total, "")
    return count


# --- checking default.xex --------------------------------------------------------------------

ENTRY_POINT = 0x82A6FBC0
# The printings of the base game seen so far, by (version, media id). Retail discs and packages come
# in several (same build, other version numbers and media ids); the header says nothing about the
# code, so only the supported printing is accepted on its header alone and any other is accepted
# when its code matches the supported one (xexcheck.py), whether it is listed here or not. The
# labels are for messages.
SUPPORTED = (7, 0x2B7A1346)
KNOWN_BASE = {
    (7, 0x2B7A1346): "Europe/Asia disc, Games on Demand package",
    (9, 0x6368CD69): "retail printing 0.0.0.9",
    (10, 0x2DC7007B): "retail printing 0.0.0.10",
    (12, 0x4000D145): "retail printing 0.0.0.12 (the one Title Update 4 is made for)",
}
REQUIRED_FILES = ("default.xex", "XMediaFacade_default.xex", "SpeechFacade_default.xex")
FLAG_PATCH = 0x60   # delta or full patch (.xexp), not a module


def xex_version(v: int) -> str:
    return f"{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def _what_differs(diffs):
    return "; ".join(diffs[:4]) + (f"; and {len(diffs) - 4} more" if len(diffs) > 4 else "")


def inspect_xex(path: Path):
    """Check a default.xex. Returns (ok, message). Accepted: the base game of Forza Horizon (title
    4D5309C9, entry point 0x82A6FBC0, version equal to its base version) whose code is the supported
    one: the supported printing (0.0.0.7, media 2B7A1346) by its header, any other printing after
    comparing its image with the supported one. Refused, with the reason: another game, a title
    update (TU4 is 0.0.4.12 on base 0.0.0.12, and its code differs; the project applies it itself),
    another executable, a damaged file, or a printing whose code is not the supported one."""
    try:
        with open(path, "rb") as f:
            raw = f.read(0x4000)
    except OSError as e:
        return False, f"Cannot read {path}: {e}"
    if raw[:4] != b"XEX2":
        return False, f"{path.name} is not an Xbox 360 executable (XEX2)."
    info = xexcheck.header_info(raw)
    if info is None:
        return False, f"{path.name} has an unusual header; it is not Forza Horizon's default.xex."
    media, version, base, title, entry, flags = info
    if f"{title:08X}" != TITLE_ID:
        return False, (f"This is not Forza Horizon: its title id is {title:08X}, not {TITLE_ID}. "
                       "Point to the game's Games on Demand package or disc (Disc 1).")
    found = (f"version {xex_version(version)}, base {xex_version(base)}, media id {media:08X}, "
             f"entry point {entry:#x}")
    if flags & FLAG_PATCH:
        return False, (f"This file is a title update patch ({found}), not the game. Choose the game's "
                       "default.xex (the base game from the disc or the plain Games on Demand package); give "
                       "the title update package under Title update.")
    if version != base:
        return False, (f"A title update is already applied to this default.xex ({found}). The launcher applies "
                       "Title Update 4 itself and needs the game without any update: the base game from your "
                       "disc or the plain Games on Demand package, with the update given under Title update.")
    if entry != ENTRY_POINT:
        return False, (f"This default.xex ({found}) is not the executable this project knows (entry point "
                       f"{ENTRY_POINT:#x}). Please report the numbers above (GitHub issues).")
    if (version, media) == SUPPORTED:
        return True, f"Forza Horizon base version ({xex_version(version)}, media id {media:08X})."
    # Another printing: the code decides.
    try:
        same, diffs, intact = xexcheck.check_code(Path(path), "default.xex")
    except (xexcheck.Unreadable, OSError) as e:
        return False, f"Cannot check {path.name}: {e}"
    if not intact:
        return False, (f"This default.xex ({found}) is damaged: its contents do not match its own checksums. "
                       "Extract the game files again.")
    if same:
        return True, (f"This is a different printing of the game (version {xex_version(version)}, media id "
                      f"{media:08X}): its code is the same as the supported one.")
    return False, (f"This default.xex ({found}) is not the supported code: it differs from the supported "
                   f"version ({xex_version(SUPPORTED[0])}) in: {_what_differs(diffs)}. Please report the numbers "
                   "above (GitHub issues, with the launcher.log); a different printing of the game may need support.")


def inspect_facade(path: Path):
    """(ok, message) for XMediaFacade_default.xex / SpeechFacade_default.xex: the supported file by
    its header, another printing's by comparing its image."""
    name = path.name
    try:
        with open(path, "rb") as f:
            raw = f.read(0x4000)
    except OSError as e:
        return False, f"Cannot read {path}: {e}"
    info = xexcheck.header_info(raw)
    if info is None:
        return False, f"{name} is not an Xbox 360 executable (XEX2)."
    media, version, base, _title, _entry, _flags = info
    if (version, base, media) == xex_reference.MODULES[name]["header"]:
        return True, ""
    try:
        same, diffs, intact = xexcheck.check_code(Path(path), name)
    except (xexcheck.Unreadable, OSError) as e:
        return False, f"Cannot check {name}: {e}"
    if not intact:
        return False, f"{name} is damaged: its contents do not match its own checksums. Extract the game files again."
    if not same:
        return False, (f"{name} (version {xex_version(version)}, media id {media:08X}) is not the "
                       f"supported code: it differs in: {_what_differs(diffs)}. Please report this (GitHub issues).")
    return True, ""


def check_facades(folder: Path):
    """(ok, message) for the facade DLLs present in a folder."""
    for n in REQUIRED_FILES[1:]:
        if (folder / n).is_file():
            ok, msg = inspect_facade(folder / n)
            if not ok:
                return False, msg
    return True, ""


def check_game_folder(folder: Path):
    """(ok, message) for an extracted game folder: the three XEX files the build needs and
    a default.xex and facades with the supported code."""
    missing = [n for n in REQUIRED_FILES if not (folder / n).is_file()]
    if missing:
        return False, (f"{folder} is missing {', '.join(missing)}. Choose the folder of the whole extracted "
                       f"game, the one that holds {', '.join(REQUIRED_FILES)} (a default.xex alone is not enough).")
    ok, msg = inspect_xex(folder / "default.xex")
    if not ok:
        return ok, msg
    fok, fmsg = check_facades(folder)
    return (True, msg) if fok else (False, fmsg)


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
