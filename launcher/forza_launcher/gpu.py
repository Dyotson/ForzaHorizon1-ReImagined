"""Which GPU the player has, and what is known to work on it (issue #52).

Many glitches are vendor specific (docs/launcher.md, "GPU choice"): the launcher detects the
graphics card, preselects NVIDIA / AMD / Intel / Steam Deck in Settings > Display, and per vendor
offers safe defaults and marks the options that are known to misbehave there, with the reason
and the issue number. The player's own choices always win: the presets are written into the
settings once (first run, or when the player changes the vendor and agrees), after which only
the player changes them; a known problem is a warning next to the option, never a restriction.

No Qt in here: the detection and the tables are plain Python and tested (test_gpu.py).
"""
import glob
import json
import os
import re
import subprocess
from dataclasses import dataclass, field

from . import cleanenv
from .paths import IS_WINDOWS

NVIDIA, AMD, INTEL, DECK = "nvidia", "amd", "intel", "deck"
VENDORS = [NVIDIA, AMD, INTEL, DECK]
LABELS = {NVIDIA: "NVIDIA", AMD: "AMD", INTEL: "Intel", DECK: "Steam Deck"}
PCI_VENDOR = {0x10DE: NVIDIA, 0x1002: AMD, 0x1022: AMD, 0x8086: INTEL}
DECK_BOARDS = {"jupiter", "galileo"}  # LCD and OLED Steam Deck (DMI board name)
# Adapters that are not a GPU the game could use
_IGNORED = ("microsoft basic", "remote", "virtual", "parsec", "citrix", "vmware", "virtio", "hyper-v",
            "mirage", "displaylink", "indirect")


def vendor_from_text(text: str) -> str:
    """The vendor an adapter name or manufacturer string stands for ('' if none of the four)."""
    t = (text or "").lower()
    if any(w in t for w in _IGNORED):
        return ""
    if any(w in t for w in ("nvidia", "geforce", "quadro", "tesla")):
        return NVIDIA
    if any(w in t for w in ("advanced micro", "amd", "radeon", "ati technologies", "ati radeon")):
        return AMD
    if "intel" in t:
        return INTEL
    return ""


def vendor_from_pci_id(vendor_id: int) -> str:
    return PCI_VENDOR.get(vendor_id, "")


def pick(vendors) -> str:
    """With several GPUs (a laptop's iGPU and dGPU) the discrete one is the one the game uses:
    NVIDIA, then AMD, then Intel."""
    for v in (NVIDIA, AMD, INTEL):
        if v in vendors:
            return v
    return ""


@dataclass
class Detected:
    vendor: str = ""    # one of VENDORS, or '' when unknown
    name: str = ""      # a description for the player ("NVIDIA GeForce RTX 3080")
    source: str = ""    # how it was found, for the launcher log


# --- Linux ---------------------------------------------------------------------------------
def _read(path: str) -> str:
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return ""


def is_steam_deck_dmi(board_name: str, board_vendor: str = "", product_name: str = "") -> bool:
    b, p = (board_name or "").strip().lower(), (product_name or "").strip().lower()
    if b in DECK_BOARDS or p in DECK_BOARDS:
        return "valve" in (board_vendor or "").lower() or not board_vendor
    return False


def detect_linux(sysfs: str = "/sys") -> Detected:
    dmi = f"{sysfs}/devices/virtual/dmi/id"
    if is_steam_deck_dmi(_read(f"{dmi}/board_name"), _read(f"{dmi}/board_vendor"), _read(f"{dmi}/product_name")):
        return Detected(DECK, "Steam Deck", "dmi board name")
    found = {}
    for card in sorted(glob.glob(f"{sysfs}/class/drm/card*")):
        if not re.fullmatch(r"card\d+", os.path.basename(card)):
            continue
        vid = _read(f"{card}/device/vendor")
        try:
            v = vendor_from_pci_id(int(vid, 16))
        except ValueError:
            continue
        if v and v not in found:
            found[v] = _read(f"{card}/device/device")
    v = pick(found)
    if not v:
        return Detected()
    return Detected(v, LABELS[v] + " graphics", "drm vendor id")


# --- Windows -------------------------------------------------------------------------------
def parse_powershell(text: str):
    """(adapter names/manufacturers, baseboard product) from the JSON the PowerShell query prints."""
    try:
        data = json.loads(text)
    except ValueError:
        return [], ""
    gpus = data.get("gpus") or []
    if isinstance(gpus, dict):
        gpus = [gpus]
    names = [" ".join(str(g.get(k) or "") for k in ("Name", "AdapterCompatibility", "PNPDeviceID")) for g in gpus]
    return [n for n in names if n.strip()], str(data.get("board") or "")


def parse_wmic_csv(text: str):
    """Rows of `wmic path win32_VideoController get AdapterCompatibility,Name /format:csv`."""
    names = []
    rows = [r for r in (ln.strip() for ln in text.splitlines()) if r]
    if not rows:
        return names
    head = [c.strip().lower() for c in rows[0].split(",")]
    for r in rows[1:]:
        cells = r.split(",")
        if len(cells) == len(head):
            d = dict(zip(head, cells))
            names.append(f"{d.get('name', '')} {d.get('adaptercompatibility', '')}")
    return names


def vendor_from_adapters(names):
    found = {}
    for n in names:
        v = vendor_from_text(n)
        if not v:
            m = re.search(r"VEN_([0-9A-F]{4})", n.upper())
            v = vendor_from_pci_id(int(m.group(1), 16)) if m else ""
        if v:
            found.setdefault(v, n.strip())
    v = pick(found)
    return v, found.get(v, "")


def _run(args, timeout=12):
    flags = 0x08000000 if IS_WINDOWS else 0   # CREATE_NO_WINDOW
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, creationflags=flags,
                          encoding="utf-8", errors="replace", env=cleanenv.clean_environment()).stdout


PS_QUERY = ("$g = @(Get-CimInstance Win32_VideoController | Select-Object Name,AdapterCompatibility,PNPDeviceID);"
            "$b = (Get-CimInstance Win32_BaseBoard).Product;"
            "ConvertTo-Json -Compress @{gpus=$g; board=$b}")


def detect_windows() -> Detected:
    names, board = [], ""
    try:
        names, board = parse_powershell(_run(["powershell", "-NoProfile", "-NonInteractive", "-Command", PS_QUERY]))
    except (OSError, subprocess.SubprocessError):
        pass
    source = "powershell"
    if not names:
        source = "wmic"
        try:
            names = parse_wmic_csv(_run(["wmic", "path", "win32_VideoController", "get",
                                         "AdapterCompatibility,Name", "/format:csv"]))
            board = board or " ".join(_run(["wmic", "baseboard", "get", "product"]).split()[1:2])
        except (OSError, subprocess.SubprocessError):
            pass
    if board.strip().lower() in DECK_BOARDS:
        return Detected(DECK, "Steam Deck", source + " baseboard")
    v, name = vendor_from_adapters(names)
    return Detected(v, name or (LABELS[v] + " graphics" if v else ""), source) if v else Detected()


def detect() -> Detected:
    """The player's GPU. FORZA_GPU=nvidia|amd|intel|deck overrides (testing, odd setups)."""
    forced = os.environ.get("FORZA_GPU", "").strip().lower()
    if forced in VENDORS:
        return Detected(forced, LABELS[forced], "FORZA_GPU")
    try:
        return detect_windows() if IS_WINDOWS else detect_linux()
    except Exception:  # detection must never stop the launcher
        return Detected()


# --- what is known per vendor ----------------------------------------------------------------
@dataclass
class Known:
    """A setting value that misbehaves on a vendor: shown as a warning while the setting has it."""
    key: str
    test: object          # callable(value) -> bool
    text: str             # what happens
    issue: int
    choice_note: str = ""  # appended to that choice's label, for choice settings (value -> note)
    choice_value: object = None


@dataclass
class Preset:
    label: str
    settings: dict        # written into the settings when the preset is applied
    summary: str          # one line for the player
    known: list = field(default_factory=list)
    notes: list = field(default_factory=list)   # (text, issue) about things no setting changes


_FSR = {"present_effect": "fsr", "resolution_scale": 1, "async_shader_compilation": True,
        "store_shaders": True}

PRESETS = {
    NVIDIA: Preset(
        "NVIDIA",
        {**_FSR, "frame_rate": 60, "gpu_backend": "vulkan"},
        "Vulkan, render scale 1 to 2 with AMD FSR 1, 60 fps, background shader compilation on.",
        [Known("gpu_backend", lambda v: v == "d3d12",
               "Direct3D 12 flickers on NVIDIA and turns reflections on glass and paint green.", 46,
               "flickers on NVIDIA, #46", "d3d12"),
         Known("resolution_scale", lambda v: int(v) >= 3,
               "Render scale 3 and above flickers and shows coloured artifacts across the map on NVIDIA; "
               "2 is the most tested.", 47),
         Known("async_shader_compilation", lambda v: not v,
               "Vulkan stutters on NVIDIA while shaders compile; keep background compilation on.", 49),
         Known("store_shaders", lambda v: not v,
               "Without the shader cache every start stutters again (worse on NVIDIA, Vulkan).", 49)],
        [("In free roam a traffic car in the distance can blow out into a cyan-white glow.", 48),
         ("Vulkan can still stutter with a warm shader cache; play a session so the cache fills.", 49)]),
    AMD: Preset(
        "AMD",
        {**_FSR, "frame_rate": 60, "gpu_backend": "vulkan"},
        "Vulkan, render scale 1 with AMD FSR 1, 60 fps. Render scale up to 4 is tested clean on Radeon.",
        [], []),
    INTEL: Preset(
        "Intel",
        {**_FSR, "frame_rate": 30, "gpu_backend": "vulkan"},
        "Vulkan, render scale 1 with AMD FSR 1, the console's 30 fps. Not tested on Intel graphics.",
        [Known("resolution_scale", lambda v: int(v) >= 2,
               "Integrated Intel graphics are slow at render scale 2 and above (not measured).", 0),
         Known("gpu_backend", lambda v: v == "d3d12",
               "Direct3D 12 takes a different render target path on Intel and was never tested.", 0,
               "untested on Intel", "d3d12")],
        [("Nobody has reported a run on Intel graphics yet; please say how it goes (GitHub issues).", 0)]),
    DECK: Preset(
        "Steam Deck",
        {**_FSR, "frame_rate": 30, "gpu_backend": "vulkan"},
        "Vulkan, render scale 1 with AMD FSR 1 and the 30 fps cap: the Deck's APU is the limit.",
        [Known("resolution_scale", lambda v: int(v) >= 2,
               "The Steam Deck runs 17 to 25 fps at render scale 1; higher scales are slower still.", 45),
         Known("frame_rate", lambda v: int(v) > 30,
               "The Steam Deck cannot hold more than the 30 fps cap (17 to 25 fps measured at 60).", 45)],
        [("The game runs at 17 to 25 fps on a Steam Deck OLED with the previous defaults; a 40 fps cap "
          "(for the 40 Hz mode) is not available, only 30 or 60 and above.", 45)]),
}


def label(vendor: str) -> str:
    return LABELS.get(vendor, "Other / not sure")


def preset(vendor: str):
    return PRESETS.get(vendor)


def settings_for(vendor: str) -> dict:
    """The values the vendor's preset sets; the Windows-only gpu_backend is dropped elsewhere."""
    p = PRESETS.get(vendor)
    if p is None:
        return {}
    out = dict(p.settings)
    if not IS_WINDOWS:
        out.pop("gpu_backend", None)
    return out


def warnings(vendor: str, settings: dict):
    """[(key, text, issue)] for the settings that hold a value known to misbehave on the vendor."""
    p = PRESETS.get(vendor)
    out = []
    if p is None:
        return out
    for k in p.known:
        if k.key == "gpu_backend" and not IS_WINDOWS:
            continue
        if k.key in settings:
            try:
                bad = k.test(settings[k.key])
            except (TypeError, ValueError):
                bad = False
            if bad:
                out.append((k.key, k.text, k.issue))
    return out


def choice_note(vendor: str, key: str, value) -> str:
    """Suffix for a choice's label ('flickers on NVIDIA, #46'), or ''."""
    p = PRESETS.get(vendor)
    if p is None:
        return ""
    for k in p.known:
        if k.key == key and k.choice_note and k.choice_value == value:
            return k.choice_note
    return ""


def issue_text(issue: int) -> str:
    return f" (#{issue})" if issue else ""


def apply_preset(settings: dict, vendor: str) -> list:
    """Write the vendor's preset into settings; returns the keys that changed."""
    changed = []
    for key, value in settings_for(vendor).items():
        if settings.get(key) != value:
            settings[key] = value
            changed.append(key)
    return changed
