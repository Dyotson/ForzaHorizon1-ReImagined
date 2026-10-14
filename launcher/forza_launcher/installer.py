"""Extracting the player's game package and DLC with tools/xcontent_extract.py and
tools/install_dlc.py (imported as they are; both only read the package)."""
import contextlib
import io
import os
import shutil
from pathlib import Path

from . import TITLE_ID, paths

paths.ensure_tools_importable()
import install_dlc  # noqa: E402
import xcontent_extract  # noqa: E402

GOD = 0x7000


class Cancelled(Exception):
    pass


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
    return p


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
    total = sum(e.size for e in entries if not e.is_dir)
    part = dest.with_name(dest.name + ".part")
    if free_space(dest.parent) < total + (1 << 28):
        raise InstallError(f"Not enough free disk space: the game needs {total / 2**30:.1f} GB.")
    shutil.rmtree(part, ignore_errors=True)
    part.mkdir(parents=True)
    done = 0
    try:
        for e in entries:
            if cancelled():
                raise Cancelled()
            progress(done, total, e.path)
            xcontent_extract.extract(pkg, e, str(part))
            done += 0 if e.is_dir else e.size
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
