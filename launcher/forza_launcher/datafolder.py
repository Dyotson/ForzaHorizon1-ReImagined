"""The folder the game is built in and keeps its data in: where the default is, whether it can be
used (writable, free space, not synced by OneDrive), what "access denied" means and how the
player's saves are carried to another folder. Issues #38 and Discussion #31."""
import ctypes
import ntpath
import os
import re
import shutil
import sys
from pathlib import Path

from . import paths

GB = 1 << 30
IS_WINDOWS = sys.platform == "win32"

# Folders that can be built again; a move leaves them behind (and starts them fresh).
REBUILDABLE = ("toolchain", "downloads", "sdk", "build", "xwin")
# What marks a folder as one of ours (a chosen folder with none of these gets a subfolder).
MARKERS = ("game", "runtime", "build", "sdk", "toolchain", "downloads", "xwin")

ACCESS_DENIED_RE = re.compile(r"GetLastError\(\) = 5\b|Access is denied|Permission denied|WinError 5\b|"
                              r"PermissionError|EACCES", re.I)

# Windows file attributes of cloud-synced files and folders (OneDrive and other sync clients).
ATTR_OFFLINE = 0x1000
ATTR_RECALL_ON_OPEN = 0x40000
ATTR_PINNED = 0x80000
ATTR_UNPINNED = 0x100000
ATTR_RECALL_ON_DATA_ACCESS = 0x400000
CLOUD_ATTRS = ATTR_OFFLINE | ATTR_RECALL_ON_OPEN | ATTR_PINNED | ATTR_UNPINNED | ATTR_RECALL_ON_DATA_ACCESS


# ---- where ----------------------------------------------------------------------------------

def legacy_data_home() -> Path:
    """Where v0.1.x kept everything on Windows."""
    return Path(os.environ.get("USERPROFILE") or Path.home()) / "Documents" / "forza"


def local_data_home() -> Path:
    """The Windows default: per user, not synced by OneDrive, not one of the folders Controlled
    folder access protects (Documents, Pictures, Desktop ...)."""
    base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
    return Path(base) / "ForzaReImagined"


def _has_content(p: Path) -> bool:
    try:
        return p.is_dir() and any(p.iterdir())
    except OSError:
        return p.is_dir()      # exists but cannot be listed: it is the player's, leave it alone


def windows_default() -> Path:
    """%LOCALAPPDATA%\\ForzaReImagined, except for an install that already lives in Documents\\forza
    (then that folder stays in use until the player moves, see offer_move)."""
    new, old = local_data_home(), legacy_data_home()
    if not new.exists() and _has_content(old):
        return old
    return new


def is_legacy_default(root: Path) -> bool:
    return IS_WINDOWS and _samepath(root, legacy_data_home())


# ---- cloud sync and access --------------------------------------------------------------------

def _norm(p) -> str:
    return ntpath.normcase(ntpath.normpath(str(p))).rstrip("\\")


def _samepath(a, b) -> bool:
    return _norm(a) == _norm(b)


def onedrive_roots() -> list:
    out = []
    for k, v in os.environ.items():
        if k.upper().startswith("ONEDRIVE") and v:
            out.append(v)
    return out


def _attrs(path) -> int | None:
    if not IS_WINDOWS:
        return None
    a = ctypes.windll.kernel32.GetFileAttributesW(str(path))
    return None if a == 0xFFFFFFFF else a


def in_onedrive(path, env_roots=None, attrs=_attrs) -> str | None:
    """Why `path` looks synced by OneDrive (or another cloud client), else None: it is under a
    OneDrive folder named by the environment, a folder on its way is called OneDrive..., or a
    folder on its way is a cloud placeholder (recall / offline / pinned attributes)."""
    roots = onedrive_roots() if env_roots is None else env_roots
    n = _norm(path)
    for r in roots:
        rn = _norm(r)
        if rn and (n == rn or n.startswith(rn + "\\")):
            return f"it is inside your OneDrive folder ({r})"
    parts = re.split(r"[\\/]+", str(path))
    for part in parts[1:]:
        if part.lower().startswith("onedrive"):
            return f"it is inside a folder named {part}"
    p = str(path)
    while p:
        a = attrs(p)
        if a is not None and a & CLOUD_ATTRS:
            return f"{p} is a cloud-synced folder (Windows marks it as OneDrive-managed)"
        parent = ntpath.dirname(p)
        if parent == p or not parent:
            break
        p = parent
    return None


def probe_writable(path: Path) -> str | None:
    """None when files can be created, written, read back and deleted in `path` (created if
    needed); else the error. This is what Controlled folder access and some antivirus programs
    deny."""
    probe = None
    try:
        Path(path).mkdir(parents=True, exist_ok=True)
        probe = Path(path) / ".forza-write-test"
        probe.write_bytes(b"x")
        if probe.read_bytes() != b"x":
            return "a file written there reads back different"
        probe.unlink()
        return None
    except OSError as e:
        if probe is not None:
            try:
                probe.unlink()
            except OSError:
                pass
        return f"{type(e).__name__}: {e}"


def suggested_folder() -> Path:
    return local_data_home() if IS_WINDOWS else paths.data_home()


def blocked_message(path, detail: str = "") -> str:
    why = in_onedrive(path)
    msg = (f"Windows would not let the launcher use the folder {path}. It is blocked by something on this "
           "computer: OneDrive (it syncs Documents, Desktop and Pictures), antivirus software, or Windows "
           "Controlled folder access (Windows Security > Virus & threat protection > Ransomware protection). "
           + (f"This folder looks synced because {why}. " if why else "")
           + f"Choose another folder in Setup (\"Where the game is built\"), for example {suggested_folder()}, "
           "then press Build again.")
    if detail:
        msg += f"\n(Windows said: {detail.strip()[:300]})"
    return msg


def is_access_denied_error(e: BaseException) -> bool:
    return isinstance(e, PermissionError) or (isinstance(e, OSError) and
                                              (getattr(e, "winerror", None) == 5 or e.errno == 13))


# ---- is a folder fit to build in -----------------------------------------------------------------

def need_gb(game_files_ready: bool, need_toolchain: bool = True) -> int:
    from . import build_env
    return build_env.NEED_DISK_GB + (build_env.NEED_DISK_TOOLCHAIN_GB if need_toolchain else 0) - (8 if game_files_ready else 0)


def check_folder(path, game_files_ready=False, extra_gb=0.0, need_toolchain=True, writable=True):
    """[(level, text)] ('error' | 'warn' | 'ok') for building in `path`."""
    from . import build_env
    path = Path(path)
    out = []
    if writable:
        err = probe_writable(path)
        if err:
            out.append(("error", f"Cannot write to {path}: {err}. {blocked_message(path)}"))
            return out
    why = in_onedrive(path)
    if why:
        out.append(("warn", f"{path}: {why}. OneDrive would upload the game's files (tens of GB), and it or Windows "
                            "may block the build. A folder outside OneDrive, such as "
                            f"{suggested_folder()}, is safer."))
    need = need_gb(game_files_ready, need_toolchain) + extra_gb
    free = build_env.free_disk(path) / GB
    out.append(("ok" if free >= need else "error",
                f"Disk: {free:.0f} GB free where the game is built, about {need:.0f} GB needed"))
    return out


def normalize_choice(path) -> Path:
    """The folder a player picked, or a subfolder of it: a folder that is not empty and is not one of
    ours gets a ForzaReImagined folder inside, so the build does not spread over the player's files."""
    p = Path(path)
    try:
        entries = os.listdir(p)
    except OSError:
        return p
    if not entries or any(m in entries for m in MARKERS):
        return p
    return p / "ForzaReImagined"


# ---- moving ------------------------------------------------------------------------------------------

def _files_under(top: Path, rel_to: Path, skip_top=()):
    out = []
    if not top.is_dir():
        return out
    for dirpath, dirs, files in os.walk(top):
        if Path(dirpath) == top:
            dirs[:] = [d for d in dirs if d not in skip_top]
        for f in files:
            out.append((os.path.join(dirpath, f), os.path.relpath(os.path.join(dirpath, f), rel_to)))
    return out


def user_data_files(old: Path, with_user: bool = True, with_game: bool = False):
    """[(source, relative)] of what is worth carrying to another folder: saves, profiles, DLC, the
    shader cache, the log and the built game (runtime/) with the build's state file (`with_user`), and
    the extracted game files (`with_game`). The downloaded toolchain, SDK and build tree are made again
    (that is also where a blocked folder fails)."""
    old = Path(old)
    out = []
    if with_user:
        out += _files_under(old, old, set(REBUILDABLE) | {"game"})
        state = old / "build" / "state.json"
        if state.is_file():
            out.append((str(state), os.path.join("build", "state.json")))
    if with_game:
        out += _files_under(old / "game", old)
    return out


def _size(files) -> int:
    total = 0
    for src, _ in files:
        try:
            total += os.path.getsize(src)
        except OSError:
            pass
    return total


def estimate_move(old: Path, with_user: bool = True, with_game: bool = False) -> int:
    return _size(user_data_files(old, with_user, with_game))


def copy_user_data(old: Path, new: Path, with_user=True, with_game=False, progress=None, cancelled=lambda: False,
                   log=None) -> int:
    """Copies (never moves or deletes: the old folder stays as it is) the user data of `old` into
    `new`. Files already there with the same size are skipped, so a repeat resumes. Returns bytes."""
    files = user_data_files(old, with_user, with_game)
    total = sum(os.path.getsize(s) for s, _ in files if os.path.exists(s)) or 1
    done = 0
    for src, rel in files:
        if cancelled():
            raise InterruptedError("cancelled")
        dst = Path(new) / rel
        size = os.path.getsize(src)
        if not (dst.is_file() and dst.stat().st_size == size):
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        done += size
        if progress:
            progress(done / total, rel)
    if log:
        log(f"copied {len(files)} files ({done / GB:.1f} GB) from {old} to {new}; the old folder is untouched")
    return done
