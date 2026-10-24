"""Mods (#149): folders of files the game reads in place of its own, laid over its archives at
start without changing them (docs/modding.md).

Each mod is a folder in <data>/mods: mod.toml (name, version, author, description, all
optional) and its files under media/, at the game's own paths. launcher.json keeps the load
order and the mods turned off ("mods": {"order": [...], "disabled": [...]}); the game gets them
as --forza_mods_dir and --forza_mods (the folders to load in load order, '|'-separated, a later
one winning over an earlier one for the same file)."""
import os
from dataclasses import dataclass
from pathlib import Path

try:
    import tomllib
except ImportError:  # Python 3.10
    tomllib = None

MANIFEST = "mod.toml"
NONE = "|"  # --forza_mods value for "no mods" (an empty list)
MAX_FILE = 0x7FFFFFFF


@dataclass
class Mod:
    folder: str
    name: str = ""
    version: str = ""
    author: str = ""
    description: str = ""
    files: int = 0
    size: int = 0
    problem: str = ""   # why the game would skip it ("" when it loads)
    note: str = ""      # a warning that does not stop it loading
    enabled: bool = True

    @property
    def title(self) -> str:
        return self.name or self.folder

    def summary(self) -> str:
        bits = [self.title]
        if self.version:
            bits.append(self.version)
        if self.author:
            bits.append(f"by {self.author}")
        return " ".join(bits)


def mods_dir(cfg) -> Path:
    return Path(cfg.data_home()) / "mods"


def read_mod(folder: Path) -> Mod:
    """A mod folder: its manifest and its files under media/, as the game will see them."""
    mod = Mod(folder=folder.name)
    manifest = folder / MANIFEST
    if manifest.is_file():
        try:
            data = manifest.read_text(encoding="utf-8-sig")
            info = tomllib.loads(data) if tomllib else {}
        except (OSError, UnicodeDecodeError, ValueError) as e:
            mod.note = f"{MANIFEST} could not be read ({e}): the folder's name is shown instead"
            info = {}
        for key in ("name", "version", "author", "description"):
            value = info.get(key)
            if isinstance(value, (str, int, float)):
                setattr(mod, key, str(value).strip())
    if "|" in folder.name:
        mod.problem = "the folder's name has a |, which the game reads as a separator: rename it"
        return mod
    media = folder / "media"
    if not media.is_dir():
        mod.problem = "no media folder: put the files under media/, at the game's paths"
        return mod
    # As the game reads the folder (forza-recomp/src/platform/mod_archive.h ScanMod): a file it
    # cannot read, or one over 2 GB, skips the mod; whole archives are left out.
    archives = 0
    try:
        for p in media.rglob("*"):
            if not p.is_file():
                continue
            rel = p.relative_to(folder).as_posix()
            if p.suffix.lower() == ".zip":
                archives += 1
                continue
            size = p.stat().st_size
            if size > MAX_FILE:
                mod.problem = f"{rel} is over 2 GB, too big for the game's archive"
                return mod
            if not os.access(p, os.R_OK):
                mod.problem = f"cannot read {rel}"
                return mod
            mod.files += 1
            mod.size += size
    except OSError as e:
        mod.problem = f"its files could not be listed ({e})"
        return mod
    if archives:
        mod.note = (f"{archives} .zip file(s) left out: whole archives are not replaced, put the files "
                    "from inside them in the mod instead")
    if not mod.files:
        mod.problem = "no files under media/"
    return mod


def scan(directory: Path) -> list:
    """The mod folders in `directory`, by name."""
    try:
        folders = sorted((p for p in Path(directory).iterdir() if p.is_dir() and not p.name.startswith(".")),
                         key=lambda p: p.name.lower())
    except OSError:
        return []
    return [read_mod(p) for p in folders]


def state(cfg) -> dict:
    s = cfg.d.get("mods")
    if not isinstance(s, dict):
        s = {}
    order = [x for x in s.get("order", []) if isinstance(x, str)]
    disabled = [x for x in s.get("disabled", []) if isinstance(x, str)]
    return {"order": order, "disabled": disabled}


def ordered(cfg, found: list) -> list:
    """`found` in the saved load order, new mods last (by name), each on unless turned off."""
    s = state(cfg)
    by_folder = {m.folder: m for m in found}
    out = [by_folder.pop(f) for f in s["order"] if f in by_folder]
    out += [m for m in found if m.folder in by_folder]
    for m in out:
        m.enabled = m.folder not in s["disabled"]
    return out


def remember(cfg, mods: list):
    """Saves the load order and the mods turned off (mods no longer there are kept, so a mod
    moved out and back keeps its place)."""
    s = state(cfg)
    shown = {m.folder for m in mods}
    order = [m.folder for m in mods] + [f for f in s["order"] if f not in shown]
    disabled = [m.folder for m in mods if not m.enabled] + [f for f in s["disabled"] if f not in shown]
    cfg.d["mods"] = {"order": order, "disabled": disabled}
    cfg.save()


def enabled(cfg) -> list:
    """The mods the game will load, in load order."""
    return [m for m in ordered(cfg, scan(mods_dir(cfg))) if m.enabled and not m.problem]


def launch_args(cfg) -> list:
    folders = [m.folder for m in enabled(cfg)]
    return [f"--forza_mods_dir={mods_dir(cfg)}", "--forza_mods=" + ("|".join(folders) or NONE)]
