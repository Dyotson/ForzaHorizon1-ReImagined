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
MAX_FILE = 0x7FFFFFFF           # the game takes no bigger file
MAX_MEMBER = (1 << 26) - 1      # the game's archive index: bigger files only replace loose files
MAX_NAME = 255                  # the game's archive names (mod_archive.h kMaxName)


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


def game_media(cfg) -> Path:
    """The game's media folder (loose files a big mod file may replace)."""
    return Path(cfg.game_files()) / "media"


def _plain(rel: str) -> bool:
    """The game's paths: printable ASCII without :*?"<>| (mod_archive.h ScanMod)."""
    return all(0x20 <= ord(c) <= 0x7E and c not in ':*?"<>|' for c in rel)


def _loose_file(media: Path | None, rel: str) -> bool:
    """Whether the game has the loose file media/<rel>, matched without case."""
    if media is None:
        return False
    here = media
    for part in rel.split("/"):
        try:
            names = {p.name.lower(): p for p in here.iterdir()}
        except OSError:
            return False
        here = names.get(part.lower())
        if here is None:
            return False
    return here.is_file()


def read_mod(folder: Path, media_of_game: Path | None = None) -> Mod:
    """A mod folder: its manifest and its files under media/, as the game will see them
    (forza-recomp/src/platform/mod_archive.h ScanMod and mod_overlay.cpp InstallMods)."""
    mod = Mod(folder=folder.name)
    if "|" in folder.name:
        mod.problem = "the folder's name has a |, which the game reads as a separator: rename it"
    elif folder.name == "*":
        mod.problem = "a folder named * would make the game load every mod: rename it"
    try:
        _read_manifest(folder, mod)
        if not mod.problem:
            _read_files(folder, mod, media_of_game)
    except OSError as e:
        mod.problem = mod.problem or f"its folder could not be read ({e})"
    return mod


def _read_manifest(folder: Path, mod: Mod):
    manifest = folder / MANIFEST
    if not manifest.is_file():
        return
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


def _read_files(folder: Path, mod: Mod, media_of_game: Path | None):
    media = folder / "media"
    if not media.is_dir():
        mod.problem = "no media folder: put the files under media/, at the game's paths"
        return
    left_out, seen, visited = [], {}, set()

    def onerror(e):
        raise e

    # Links are followed, as the game does; names are where the files are listed.
    for dirpath, dirnames, filenames in os.walk(media, followlinks=True, onerror=onerror):
        st = os.stat(dirpath)
        if (st.st_dev, st.st_ino) in visited:  # a link back up the tree
            dirnames[:] = []
            continue
        visited.add((st.st_dev, st.st_ino))
        for name in filenames:
            p = Path(dirpath) / name
            rel = Path(os.path.relpath(p, media)).as_posix()
            shown = f"media/{rel}"
            if not p.is_file():
                continue
            if not _plain(rel):
                left_out.append(f"{shown}: the game's paths are plain ASCII")
                continue
            game_name = rel.lower()
            if len(game_name.encode()) > MAX_NAME:
                left_out.append(f"{shown}: its path is over 255 characters")
                continue
            if game_name.endswith(".zip"):
                left_out.append(f"{shown}: whole archives are not replaced, put the files from inside it in the mod")
                continue
            size = p.stat().st_size
            if size > MAX_FILE:
                mod.problem = f"{shown} is over 2 GB, too big for the game"
                return
            if size > MAX_MEMBER and not _loose_file(media_of_game, rel):
                mod.problem = (f"{shown} is over 64 MB, more than the game's archives hold, and the game "
                               "has no loose file of that name for it to replace")
                return
            if not os.access(p, os.R_OK):
                mod.problem = f"cannot read {shown}"
                return
            if game_name in seen:
                mod.problem = f"two files differ only in case: {shown} and media/{seen[game_name]}"
                return
            seen[game_name] = rel
            mod.files += 1
            mod.size += size
    if left_out:
        note = f"{len(left_out)} file(s) left out: " + "; ".join(left_out[:3]) + ("; ..." if len(left_out) > 3 else "")
        mod.note = f"{mod.note}\n{note}" if mod.note else note
    if not mod.files:
        mod.problem = "no files under media/ the game can use"


def scan(directory: Path, media_of_game: Path | None = None) -> list:
    """The mod folders in `directory`, by name."""
    try:
        folders = sorted((p for p in Path(directory).iterdir() if not p.name.startswith(".") and p.is_dir()),
                         key=lambda p: p.name.lower())
    except OSError:
        return []
    return [read_mod(p, media_of_game) for p in folders]


def state(cfg) -> dict:
    s = cfg.d.get("mods")
    if not isinstance(s, dict):
        s = {}
    def names(key):
        value = s.get(key)  # a hand-edited launcher.json may hold anything here
        return [x for x in value if isinstance(x, str)] if isinstance(value, list) else []
    return {"order": names("order"), "disabled": names("disabled")}


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
    return [m for m in ordered(cfg, scan(mods_dir(cfg), game_media(cfg))) if m.enabled and not m.problem]


def launch_args(cfg, named=()) -> list:
    """The game's mod options, leaving out those the player names in the extra flags
    (`named`: option names without dashes)."""
    args = []
    if "forza_mods_dir" not in named:
        args.append(f"--forza_mods_dir={mods_dir(cfg)}")
    if "forza_mods" not in named:
        args.append("--forza_mods=" + ("|".join(m.folder for m in enabled(cfg)) or NONE))
    return args
