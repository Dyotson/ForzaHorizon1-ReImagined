"""Where things live: the launcher's config, the game's user data, the game files and binary."""
import os
import sys
from pathlib import Path

IS_WINDOWS = sys.platform == "win32"
# Running inside the Flatpak sandbox (flatpak/): the runtime is org.freedesktop.Sdk, so the C++
# headers, cmake, ninja and python are in /usr, and the LLVM extension is mounted below.
FLATPAK_ID = os.environ.get("FLATPAK_ID", "")
IN_FLATPAK = bool(FLATPAK_ID) or Path("/.flatpak-info").exists()
FROZEN = bool(getattr(sys, "frozen", False))
PKG_DIR = Path(__file__).resolve().parent
# launcher/ in a source checkout; the PyInstaller bundle directory when frozen.
LAUNCHER_ROOT = Path(getattr(sys, "_MEIPASS", PKG_DIR.parent))
REPO_ROOT = PKG_DIR.parent.parent  # only meaningful in a source checkout


def app_dir() -> Path:
    """The directory the launcher executable is in (the game's dist layout)."""
    if FROZEN:
        return Path(sys.executable).resolve().parent
    return PKG_DIR.parent


def flatpak_llvm_dirs() -> list:
    """bin/ of the org.freedesktop.Sdk.Extension.llvm* extensions mounted in this sandbox (newest
    first): where the app's own extension point or the Sdk's puts them."""
    out = []
    for base in ("/app/lib/sdk", "/usr/lib/sdk"):
        try:
            names = sorted((n for n in os.listdir(base) if n.startswith("llvm")),
                           key=lambda n: int(n[4:]) if n[4:].isdigit() else 0, reverse=True)
        except OSError:
            continue
        out += [f"{base}/{n}/bin" for n in names if os.path.isfile(f"{base}/{n}/bin/clang")]
    return out


def in_checkout() -> bool:
    return not FROZEN and (REPO_ROOT / "tools" / "xcontent_extract.py").is_file()


def config_dir() -> Path:
    """Per-user launcher config (launcher.json)."""
    if IS_WINDOWS:
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
        return base / "ForzaReImagined"
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "forza-reimagined"


def data_home() -> Path:
    """The game's user data root: saves, DLC, online identity, shader cache, forza.log.
    Matches play.sh (~/.local/share/forza) and play.bat (Documents\\forza)."""
    if IS_WINDOWS:
        return Path(os.environ.get("USERPROFILE") or Path.home()) / "Documents" / "forza"
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "forza"


def default_game_files() -> Path:
    """Where the game's files are extracted to."""
    return data_home() / "game"


def asset(name: str) -> Path:
    """A launcher image: bundled in the frozen app, else docs/images of the checkout."""
    for base in (LAUNCHER_ROOT / "assets", REPO_ROOT / "docs" / "images"):
        if (base / name).is_file():
            return base / name
    return LAUNCHER_ROOT / "assets" / name


def tools_dir() -> Path | None:
    """The directory holding xcontent_extract.py and install_dlc.py."""
    for d in (LAUNCHER_ROOT / "tools", app_dir(), REPO_ROOT / "tools"):
        if (d / "xcontent_extract.py").is_file() and (d / "install_dlc.py").is_file():
            return d
    return None


def ensure_tools_importable() -> None:
    d = tools_dir()
    if d is not None and str(d) not in sys.path:
        sys.path.insert(0, str(d))
