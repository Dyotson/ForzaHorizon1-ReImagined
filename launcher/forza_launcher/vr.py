"""The "VR headset (experimental)" setting (vr_mode, docs/vr.md, #333): what a start needs besides the cvar.

The game opens the OpenXR loader only when vr_mode is not off. The loader finds the runtime from
the active_runtime.json the runtime installs (WiVRn, Monado and SteamVR write
~/.config/openxr/1/active_runtime.json). Inside the Flatpak, XDG_CONFIG_HOME is the app's own
folder, so the loader would not look there: the launcher names the host's manifest with
XR_RUNTIME_JSON (the sandbox can read it, flatpak/ manifest's finish-args).
"""
import os
from pathlib import Path

from . import paths

MODES = {"screen": "Virtual screen", "cockpit": "Cockpit"}
# "VR picture" presets for the cockpit (docs/vr.md, "Version 3"): the draw resolution scale across and
# up. The cockpit view is 123 x 109 degrees drawn into the game's 16:9 picture, so its pixels per
# degree are 6.1 across and 4.5 up per unit of scale: 2 x 3 gives 12.1 x 13.4, 3 x 4 18.2 x 17.9.
PICTURE_SCALES = {"sharp": (2, 3), "sharpest": (3, 4)}
LOG_HINT = "lines starting with \"VR:\" in forza.log say what happened"


def enabled(cfg) -> bool:
    return cfg.settings.get("vr_mode", "off") in MODES


def active_runtime_manifest(home: Path, environ=None):
    """The runtime manifest the OpenXR loader picks on the host, or None: XDG_CONFIG_HOME (the
    host's, ~/.config by default), then the XDG_CONFIG_DIRS and /etc."""
    environ = os.environ if environ is None else environ
    config_home = Path(environ.get("HOST_XDG_CONFIG_HOME") or home / ".config")
    dirs = [config_home] + [Path(d) for d in (environ.get("HOST_XDG_CONFIG_DIRS") or "/etc/xdg").split(":") if d]
    dirs.append(Path("/etc"))
    for d in dirs:
        manifest = d / "openxr" / "1" / "active_runtime.json"
        if manifest.exists():
            return manifest
    return None


def configure(cfg, env: dict, args: list, notes: list, in_flatpak=None, home=None, environ=None):
    """Arguments, environment and a note for a start with the VR setting on."""
    if not enabled(cfg):
        return
    mode = cfg.settings["vr_mode"]
    environ = os.environ if environ is None else environ
    in_flatpak = paths.IN_FLATPAK if in_flatpak is None else in_flatpak
    if paths.IS_WINDOWS:
        # The OpenXR path is Vulkan only (XR_KHR_vulkan_enable2).
        args[:] = [a for a in args if not a.startswith("--gpu_backend=")]
        args.append("--gpu_backend=vulkan")
    if in_flatpak and "XR_RUNTIME_JSON" not in env and "XR_RUNTIME_JSON" not in environ:
        manifest = active_runtime_manifest(Path(home or Path.home()), environ)
        if manifest is not None:
            env["XR_RUNTIME_JSON"] = str(manifest)
    picture = cfg.settings.get("vr_picture", "game")
    if mode == "cockpit" and picture in PICTURE_SCALES:
        if cfg.settings.get("present_effect") == "fsr3" or cfg.settings.get("fh1_upscaler", "off") != "off":
            notes.append("VR picture: AMD FSR 3 and NVIDIA DLSS keep their own scale; the VR picture "
                         "preset is not applied.")
        else:
            x, y = PICTURE_SCALES[picture]
            args[:] = [a for a in args if not a.startswith(("--resolution_scale=", "--draw_resolution_scale_"))]
            args += [f"--draw_resolution_scale_x={x}", f"--draw_resolution_scale_y={y}"]
    notes.append(f"VR headset: {MODES[mode]}. Start your OpenXR runtime and connect the headset "
                 f"before the game; without them it starts on the screen only ({LOG_HINT}).")
