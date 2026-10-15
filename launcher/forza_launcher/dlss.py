"""The optional NVIDIA runtime, downloaded from NVIDIA's pinned public SDK.

No system-wide installation or DLL search-path changes. The renderer receives
the private runtime folder for NGX's explicit feature search path.
"""
import os
import json
import shutil
import tempfile
import uuid
from pathlib import Path

from . import fetch, paths, pins

SDK_REVISION = "374959484e79a640feaba44c93ac8cfb0a03f5b5"
_BASE = f"https://raw.githubusercontent.com/NVIDIA/DLSS/{SDK_REVISION}"
RUNTIME_SHA256 = "3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983"
LICENSE_SHA256 = "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"

# fetch.download uses the same checked/resumable path as the toolchain installer.
pins.DOWNLOADS.update({
    "dlss-runtime-windows": (
        f"{_BASE}/lib/Windows_x86_64/rel/nvngx_dlss.dll", RUNTIME_SHA256, "file"),
    "dlss-license": (f"{_BASE}/LICENSE.txt", LICENSE_SHA256, "file"),
})


def runtime_dir(cfg) -> Path:
    return cfg.data_home() / "graphics" / "nvidia-dlss" / SDK_REVISION


def enabled(cfg) -> bool:
    return cfg.settings.get("fh1_upscaler", "off") != "off"


def check_renderer(folder):
    """An older SDK overlay must not silently ignore the native settings."""
    try:
        folder = Path(folder)
        capabilities = json.loads((folder / "fh1-native-dlss.json").read_text(encoding="utf-8"))
        valid = (isinstance(capabilities, dict) and capabilities.get("api") == 1 and
                 capabilities.get("renderer_sha256") == fetch.sha256_of(folder / "rexgpu-xenos.dll"))
    except (OSError, ValueError):
        valid = False
    if not valid:
        raise RuntimeError("This game build does not include the native DLSS renderer. "
                           "Rebuild with this branch's updated SDK, or turn NVIDIA DLSS off.")


def configure(cfg, env, args, notes, write=True):
    if not enabled(cfg):
        return
    mode = cfg.settings.get("fh1_upscaler")
    if mode not in ("dlss", "dlaa"):
        raise RuntimeError(f"Unknown NVIDIA reconstruction mode: {mode}")
    if not paths.IS_WINDOWS:
        raise RuntimeError("Native DLSS currently supports Windows only.")
    if write:
        prepare(cfg)
    env["FORZA_DLSS_RUNTIME"] = str(runtime_dir(cfg))
    status = cfg.data_home() / "logs" / f"nvidia-dlss-{uuid.uuid4().hex}.status"
    if write:
        status.parent.mkdir(parents=True, exist_ok=True)
    env["FORZA_DLSS_STATUS_FILE"] = str(status)
    args += ["--gpu_backend=vulkan", "--resolution=1440p", "--resolution_scale=2"]
    notes.append("DLAA: native 1440p world and UI." if mode == "dlaa" else
                 "DLSS: 720p world → 1440p, native 1440p UI.")


def session_status(plan):
    """The renderer reports readiness only after its first successful NGX frame."""
    path = plan.env.get("FORZA_DLSS_STATUS_FILE")
    if not path:
        return None
    try:
        text = Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return None
    if text.startswith(("ready: ", "error: ")):
        return text.startswith("error: "), text.split(": ", 1)[1]
    return None


def ready(cfg) -> bool:
    folder = runtime_dir(cfg)
    try:
        return (fetch.sha256_of(folder / "nvngx_dlss.dll") == RUNTIME_SHA256 and
                fetch.sha256_of(folder / "LICENSE.txt") == LICENSE_SHA256)
    except OSError:
        return False


def prepare(cfg, progress=lambda done, total: None, cancelled=lambda: False) -> Path:
    """Fetch on first use, repair changed files, and reuse verified files offline."""
    if not paths.IS_WINDOWS:
        raise RuntimeError("This DLSS integration currently supports Windows only.")
    dest = runtime_dir(cfg)
    if ready(cfg):
        return dest
    cache = cfg.data_home() / "downloads" / "nvidia-dlss" / SDK_REVISION
    assets = [("dlss-runtime-windows", "nvngx_dlss.dll"), ("dlss-license", "LICENSE.txt")]
    # Verify every asset before replacing any installed file.
    sources = [(fetch.download(key, cache, progress, cancelled), name) for key, name in assets]
    dest.mkdir(parents=True, exist_ok=True)
    for source, name in sources:
        if cancelled():
            raise fetch.Cancelled()
        fd, temporary = tempfile.mkstemp(dir=dest, prefix=name + ".")
        try:
            with os.fdopen(fd, "wb") as out, open(source, "rb") as incoming:
                shutil.copyfileobj(incoming, out)
            os.replace(temporary, dest / name)
        finally:
            Path(temporary).unlink(missing_ok=True)
    return dest
