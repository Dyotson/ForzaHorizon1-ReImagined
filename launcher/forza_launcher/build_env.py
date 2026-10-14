"""What the machine has: memory, cores, disk, and the tools the build needs."""
import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from . import paths, pins

MB = 1 << 20
GB = 1 << 30


def ram_bytes():
    """(total, available) physical memory in bytes."""
    if sys.platform == "win32":
        class MS(ctypes.Structure):
            _fields_ = [("l", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong), ("tpf", ctypes.c_ulonglong),
                        ("apf", ctypes.c_ulonglong), ("tv", ctypes.c_ulonglong),
                        ("av", ctypes.c_ulonglong), ("ex", ctypes.c_ulonglong)]
        m = MS()
        m.l = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.total, m.avail
    total = avail = 0
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    total = int(line.split()[1]) * 1024
                elif line.startswith("MemAvailable:"):
                    avail = int(line.split()[1]) * 1024
    except OSError:
        pass
    return total, avail


def free_disk(path: Path) -> int:
    p = Path(path)
    while not p.exists() and p != p.parent:
        p = p.parent
    return shutil.disk_usage(p).free


# Rough needs, from a build of this project: the extracted game 7.6 GB, the build tree about
# 4 GB, the SDK 1.5 GB, and a downloaded toolchain 6 GB more.
NEED_DISK_GB = 15
NEED_DISK_TOOLCHAIN_GB = 3
NEED_RAM_GB = 8
RAM_PER_JOB_GB = 1.5


def default_jobs():
    cores = os.cpu_count() or 2
    _total, avail = ram_bytes()
    by_ram = int(avail / GB / RAM_PER_JOB_GB) if avail else cores
    return max(1, min(cores, by_ram, 16))


def preflight(root: Path, game_files_ready: bool, need_toolchain: bool):
    """[(level, text)] with level 'error', 'warn' or 'ok'."""
    out = []
    total, avail = ram_bytes()
    if total:
        text = f"Memory: {total / GB:.0f} GB installed, {avail / GB:.0f} GB free"
        lvl = "ok"
        if total < NEED_RAM_GB * GB:
            lvl, text = "error", text + f" (the build and the game need about {NEED_RAM_GB} GB)"
        elif avail < NEED_RAM_GB * GB:
            lvl, text = "warn", text + f" (close other programs; the build and the game need about {NEED_RAM_GB} GB)"
        out.append((lvl, text))
    need = NEED_DISK_GB + (NEED_DISK_TOOLCHAIN_GB if need_toolchain else 0) - (8 if game_files_ready else 0)
    free = free_disk(root) / GB
    out.append(("ok" if free >= need else "error",
                f"Disk: {free:.0f} GB free where the game is built, about {need} GB needed"))
    cores = os.cpu_count() or 1
    out.append(("ok" if cores >= 4 else "warn",
                f"CPU: {cores} cores" + ("" if cores >= 4 else " (the build will take hours)")))
    return out


@dataclass
class Tools:
    cc: str = ""
    cxx: str = ""
    cmake: str = ""
    ninja: str = ""
    path_dirs: list = field(default_factory=list)  # prepended to PATH
    source: dict = field(default_factory=dict)      # tool -> "system" | "downloaded"
    missing: list = field(default_factory=list)     # download names still needed
    problems: list = field(default_factory=list)    # things only the user can fix


def _version_of(cmd, pattern):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(pattern, out)
    return tuple(int(x) for x in m.group(1).split(".")) if m else None


def _plat():
    return "windows" if sys.platform == "win32" else "linux"


def toolchain_dir(root: Path) -> Path:
    return root / "toolchain"


def downloaded_paths(root: Path):
    """Where the downloaded tools unpack to (whether or not they are there)."""
    p = _plat()
    t = toolchain_dir(root)
    exe = ".exe" if p == "windows" else ""
    llvm = t / f"llvm-{pins.LLVM_VERSION}"
    cm = t / f"cmake-{pins.CMAKE_VERSION}"
    return {
        "llvm": llvm / "bin",
        "clang": llvm / "bin" / ("clang-cl" + exe if p == "windows" else "clang"),
        "cmake": cm / "bin" / ("cmake" + exe),
        "ninja": t / f"ninja-{pins.NINJA_VERSION}" / ("ninja" + exe),
    }


_CXX_CACHE = {}
_TOOLS_CACHE = {}


def cxx_works(cxx: str, env=None) -> str | None:
    """None when the compiler builds C++23 with the standard library; else why not. (Cached: it
    runs the compiler.)"""
    if sys.platform == "win32":
        return None
    if cxx not in _CXX_CACHE:
        _CXX_CACHE[cxx] = _cxx_works(cxx, env)
    return _CXX_CACHE[cxx]


def _cxx_works(cxx: str, env=None) -> str | None:
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / "t.cpp"
        src.write_text('#include <format>\n#include <string>\n#include <thread>\n'
                       'int main(){ return std::format("{}", 1).size() == 1 ? 0 : 1; }\n')
        r = subprocess.run([cxx, "-std=c++23", str(src), "-o", str(Path(d) / "t")],
                           capture_output=True, text=True, env=env, timeout=120)
        if r.returncode != 0:
            return (r.stderr.strip().splitlines() or ["compile failed"])[-1]
    return None


def detect_tools(root: Path, prefer_downloaded=False, max_age=5.0) -> Tools:
    """Cached for a few seconds: the window asks often. Pass max_age=0 to look again."""
    import time
    key = (str(root), prefer_downloaded)
    hit = _TOOLS_CACHE.get(key)
    if hit and time.time() - hit[0] < max_age:
        return hit[1]
    tools = _detect_tools(root, prefer_downloaded)
    _TOOLS_CACHE[key] = (time.time(), tools)
    return tools


def _flatpak_path():
    """Inside the Flatpak the compiler comes with the SDK's LLVM extension (not on PATH by itself)."""
    if paths.IN_FLATPAK:
        have = os.environ.get("PATH", "").split(os.pathsep)
        extra = [d for d in paths.flatpak_llvm_dirs() if d not in have]
        if extra:
            os.environ["PATH"] = os.pathsep.join(extra + have)


def _detect_tools(root: Path, prefer_downloaded=False) -> Tools:
    t = Tools()
    _flatpak_path()
    p = _plat()
    dp = downloaded_paths(root)
    exe = ".exe" if p == "windows" else ""

    # compiler
    sys_clang = None if prefer_downloaded else shutil.which("clang-cl" if p == "windows" else "clang")
    ver = _version_of([sys_clang, "--version"], r"clang version (\d+\.\d+)") if sys_clang else None
    if sys_clang and ver and ver[0] >= pins.MIN_SYSTEM_CLANG:
        t.cc = sys_clang
        t.cxx = shutil.which("clang++") or sys_clang if p != "windows" else sys_clang
        t.source["clang"] = f"system (clang {ver[0]}.{ver[1]})"
    elif dp["clang"].is_file():
        t.cc = str(dp["clang"])
        t.cxx = str(dp["llvm"] / ("clang-cl" + exe if p == "windows" else "clang++"))
        t.path_dirs.append(str(dp["llvm"]))
        t.source["clang"] = f"downloaded (LLVM {pins.LLVM_VERSION})"
    else:
        t.missing.append(f"llvm-{p}")
    # cmake
    sys_cmake = None if prefer_downloaded else shutil.which("cmake")
    cv = _version_of([sys_cmake, "--version"], r"version (\d+\.\d+)") if sys_cmake else None
    if sys_cmake and cv and cv >= (3, 25):
        t.cmake = sys_cmake
        t.source["cmake"] = f"system (cmake {cv[0]}.{cv[1]})"
    elif dp["cmake"].is_file():
        t.cmake = str(dp["cmake"])
        t.path_dirs.append(str(dp["cmake"].parent))
        t.source["cmake"] = f"downloaded (cmake {pins.CMAKE_VERSION})"
    else:
        t.missing.append(f"cmake-{p}")
    # ninja
    sys_ninja = None if prefer_downloaded else shutil.which("ninja")
    if sys_ninja:
        t.ninja = sys_ninja
        t.source["ninja"] = "system"
    elif dp["ninja"].is_file():
        t.ninja = str(dp["ninja"])
        t.path_dirs.append(str(dp["ninja"].parent))
        t.source["ninja"] = f"downloaded (ninja {pins.NINJA_VERSION})"
    else:
        t.missing.append(f"ninja-{p}")
    # the C++ standard library headers (Linux): only the system can supply them
    if t.cxx and p == "linux":
        env = dict(os.environ)
        env["PATH"] = os.pathsep.join(t.path_dirs + [env.get("PATH", "")])
        why = cxx_works(t.cxx, env)
        if why:
            t.problems.append(
                "The compiler cannot build a C++23 program (" + why + "). Install your system's "
                "C++ development packages: g++ and libstdc++ headers (Debian/Ubuntu: build-essential; "
                "Fedora: gcc-c++; Arch: gcc), then try again.")
    return t


def tool_env(tools: Tools):
    env = dict(os.environ)
    if tools.path_dirs:
        env["PATH"] = os.pathsep.join(tools.path_dirs + [env.get("PATH", "")])
    return env
