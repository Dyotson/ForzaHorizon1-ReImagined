"""The launcher's own configuration (launcher.json in the per-user config dir) and the
forza.toml it generates from it."""
import json
import os
import sys
import tempfile
from pathlib import Path

from . import PUBLIC_SERVER, paths, schema

ONLINE_MODES = ["public", "local", "custom", "offline"]
# First-run values that differ from the game's own defaults.
FIRST_RUN = {"frame_rate": 60}


def _default():
    return {
        "version": 1,
        "game_package": "",       # the Games on Demand package (file)
        "tu_package": "",         # the player's title update package (TU4)
        "dlc_dir": "",            # folder of DLC packages (4D5309C9/00000002)
        "game_root": "",          # an already extracted folder to use instead of data/game
        "dlc_installed": 0,
        "online": {"mode": "public", "custom_server": ""},
        "profile": {"mode": "own", "xenia_content": "", "xenia_xuid": ""},
        "advanced": {"game_dir": "", "data_root": "", "extra_args": "", "extra_env": "",
                     "extras_toml": "", "pad_per_user": False},
        "settings": {**schema.defaults(), **FIRST_RUN},
    }


def _merge(base, over):
    for k, v in over.items():
        if isinstance(base.get(k), dict) and isinstance(v, dict):
            _merge(base[k], v)
        elif k in base:
            base[k] = v
    return base


def _atomic_write(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class Config:
    def __init__(self, path: Path | None = None):
        self.path = path or paths.config_dir() / "launcher.json"
        self.d = _default()
        self.loaded = False
        self.load()

    # --- persistence ---
    def load(self):
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        if isinstance(data, dict):
            _merge(self.d, data)
            self.loaded = True
        for key, setting in schema.BY_KEY.items():
            self.d["settings"][key] = schema.coerce(setting, self.d["settings"].get(key))

    def save(self):
        _atomic_write(self.path, json.dumps(self.d, indent=2) + "\n")

    # --- accessors ---
    def __getitem__(self, key):
        return self.d[key]

    def __setitem__(self, key, value):
        self.d[key] = value

    @property
    def settings(self):
        return self.d["settings"]

    @property
    def online(self):
        return self.d["online"]

    @property
    def profile(self):
        return self.d["profile"]

    @property
    def advanced(self):
        return self.d["advanced"]

    def data_home(self) -> Path:
        override = self.advanced.get("data_root", "").strip()
        return Path(override) if override else paths.data_home()

    def game_files(self) -> Path:
        override = self.d.get("game_root", "").strip()
        return Path(override) if override else self.data_home() / "game"

    def game_files_ready(self) -> bool:
        return (self.game_files() / "default.xex").is_file()

    def server_spec(self):
        """The Forza Live server the game should use: 'host[:port]', or '' for none."""
        mode = self.online["mode"]
        if mode == "public":
            return PUBLIC_SERVER
        if mode == "custom":
            return self.online.get("custom_server", "").strip()
        return ""


# --- forza.toml -----------------------------------------------------------------------

def toml_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(round(v, 6))
    return json.dumps(str(v))  # a TOML basic string


def parse_toml(text: str) -> dict:
    import tomllib
    return tomllib.loads(text)


def flatten(table: dict, prefix="") -> dict:
    """Nested tables join their names with '_' (a [gpu] table's vsync is gpu_vsync): the SDK's rule."""
    out = {}
    for k, v in table.items():
        key = prefix + k
        if isinstance(v, dict):
            out.update(flatten(v, key + "_"))
        else:
            out[key] = v
    return out


def render_toml(cfg: Config) -> str:
    lines = ["# Written by the Forza Horizon 1: ReImagined launcher. Change settings there;",
             "# keys added under 'Other settings' (Advanced) are kept."]
    for s in schema.S:
        if not s.applies():
            continue
        v = schema.coerce(s, cfg.settings.get(s.key, s.default))
        if v != s.default:
            lines.append(f"{s.key} = {toml_value(v)}")
    # public and custom servers are named here too; local (an environment variable at start)
    # and offline leave live_server empty.
    server = cfg.server_spec()
    if server:
        lines.append(f"live_server = {toml_value(server)}")
    extras = cfg.advanced.get("extras_toml", "").strip()
    if extras:
        lines += ["", "# Other settings", extras]
    return "\n".join(lines) + "\n"


def validate_extras(text: str) -> str | None:
    """An error message when the 'other settings' text is not valid TOML, else None."""
    if not text.strip():
        return None
    try:
        parse_toml(text)
    except Exception as e:  # tomllib.TOMLDecodeError
        return str(e)
    return None


def import_toml(cfg: Config, text: str) -> None:
    """Adopt an existing forza.toml (first launch): known keys into settings, the rest kept."""
    try:
        flat = flatten(parse_toml(text))
    except Exception:
        return
    extras = []
    for key, val in flat.items():
        s = schema.BY_KEY.get(key)
        if s is not None:
            cfg.settings[key] = schema.coerce(s, val)
        elif key == "live_server":
            if isinstance(val, str) and val:
                if val == PUBLIC_SERVER:
                    cfg.online["mode"] = "public"
                else:
                    cfg.online["mode"], cfg.online["custom_server"] = "custom", val
            else:
                cfg.online["mode"] = "local"
        elif isinstance(val, (bool, int, float, str)):
            extras.append(f"{key} = {toml_value(val)}")
    if extras:
        cfg.advanced["extras_toml"] = "\n".join(extras)


def write_toml(cfg: Config, toml_path: Path) -> str:
    text = render_toml(cfg)
    try:
        if toml_path.is_file() and toml_path.read_text(encoding="utf-8") == text:
            return text
    except OSError:
        pass
    _atomic_write(toml_path, text)
    return text
