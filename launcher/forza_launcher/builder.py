"""Building the game on the player's machine: toolchain, SDK, codegen from their own
default.xex, CMake configure and ninja. Nothing built here is ever uploaded or shipped.

Layout under the data root (default ~/.local/share/forza, Documents\\forza on Windows):
  game/        the extracted game files (default.xex ...)
  toolchain/   downloaded LLVM, CMake, ninja (only when the system has none)
  downloads/   the pinned archives (kept, so a retry needs no download)
  sdk/         the patched ReXGlue SDK
  build/       the project sources, generated code and the build tree
  runtime/     what Play runs: the program, its libraries, forza.toml
"""
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import __version__, paths, pins
from . import build_env, fetch
from .fetch import Cancelled

IS_WIN = sys.platform == "win32"
PLAT = "windows" if IS_WIN else "linux"
EXE = ".exe" if IS_WIN else ""


class BuildError(Exception):
    pass


class Reporter:
    """Receives what the build does. The window and the command line each implement it."""

    def log(self, text): ...
    def status(self, text): ...
    def progress(self, fraction, detail=""): ...   # 0..1 over the whole build
    def cancelled(self): return False


@dataclass
class Options:
    root: Path
    jobs: int = 0
    low_priority: bool = True
    accept_ms_license: bool = False
    prefer_downloaded: bool = False
    clean: bool = False
    title_update: str = ""   # the player's TU4 package (optional)

    def __post_init__(self):
        self.root = Path(self.root)
        if not self.jobs:
            self.jobs = build_env.default_jobs()


# ---- locations ----------------------------------------------------------------------------

def project_dir() -> Path | None:
    """The shipped project sources: a folder holding forza-recomp/ (and tools/cmake)."""
    for d in (paths.app_dir() / "project", paths.LAUNCHER_ROOT / "project", paths.REPO_ROOT):
        if (d / "forza-recomp" / "CMakeLists.txt").is_file():
            return d
    return None


def sdk_overlay_archive() -> Path | None:
    """The patched SDK pieces CI builds (Linux: plugin, runtime, codegen tool; Windows: the whole
    SDK prefix), shipped next to the launcher."""
    name = f"rexglue-overlay-{PLAT}-amd64.tar.gz"
    for d in (paths.app_dir() / "sdk-overlay", paths.LAUNCHER_ROOT / "sdk-overlay", paths.app_dir()):
        if (d / name).is_file():
            return d / name
    return None


def dev_sdk_overlay() -> Path | None:
    """A source checkout's own patched overlay (tools/build_rexglue_sdk.sh), for development."""
    if IS_WIN or not paths.in_checkout():
        return None
    c = Path(os.environ.get("REXSDK_PATCHED", Path.home() / "rexglue-sdk-patched-forza/0.10.0.24/linux-amd64"))
    return c if (c / "lib" / "librexgpu-xenos.so").is_file() else None


def sdk_dir(root: Path) -> Path:
    return root / "sdk" / f"{pins.SDK_VERSION}-{PLAT}"


def build_dir(root: Path) -> Path:
    return root / "build"


def runtime_dir(root: Path) -> Path:
    return root / "runtime"


EXCLUDE_DIRS = {"out", "__pycache__"}


def source_files(proj: Path):
    """The project files the build uses: forza-recomp minus generated code and output."""
    base = proj / "forza-recomp"
    for dirpath, dirs, files in os.walk(base):
        rel = Path(dirpath).relative_to(base)
        dirs[:] = sorted(d for d in dirs if d not in EXCLUDE_DIRS and
                         not (rel == Path(".") and d == "generated"))
        for f in sorted(files):
            if f.endswith((".pyc", ".log")):
                continue
            yield rel / f
    if (base / "generated" / "rexglue.cmake").is_file():
        yield Path("generated") / "rexglue.cmake"


def sources_hash(proj: Path) -> str:
    h = hashlib.sha1()
    for rel in sorted(source_files(proj)):
        h.update(str(rel).replace("\\", "/").encode())
        h.update((proj / "forza-recomp" / rel).read_bytes())
    cm = proj / "tools" / "cmake" / "windows-native-clang-cl.cmake"
    if cm.is_file():
        h.update(cm.read_bytes())
    return h.hexdigest()


def current_build_id(root: Path, title_update: str = "") -> str | None:
    proj = project_dir()
    if proj is None:
        return None
    tu = ""
    if title_update and Path(title_update).is_file():
        tu = str(Path(title_update).stat().st_size)
    return hashlib.sha1(f"{__version__}|{sources_hash(proj)}|{pins.SDK_TAG}|{PLAT}|{tu}".encode()).hexdigest()[:16]


def read_state(root: Path) -> dict:
    try:
        return json.loads((build_dir(root) / "state.json").read_text())
    except (OSError, ValueError):
        return {}


def write_state(root: Path, state: dict):
    build_dir(root).mkdir(parents=True, exist_ok=True)
    (build_dir(root) / "state.json").write_text(json.dumps(state, indent=2))


def build_status(root: Path, title_update: str = ""):
    """('built' | 'stale' | 'none' | 'nosource', detail)"""
    cid = current_build_id(root, title_update)
    if cid is None:
        return "nosource", "The project sources are not installed next to the launcher."
    st = read_state(root)
    rt = runtime_dir(root) / ("forza" + EXE)
    if st.get("complete") and rt.is_file():
        if st.get("id") == cid:
            return "built", f"Built {st.get('when', '')}"
        return "stale", "A new version of the project needs a rebuild."
    if st.get("started"):
        return "partial", "A build was started and not finished; it resumes where it stopped."
    return "none", "The game has not been built yet."


# ---- running programs ------------------------------------------------------------------------

def kill_tree(proc):
    if proc.poll() is not None:
        return
    try:
        if IS_WIN:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
        else:
            os.killpg(proc.pid, signal.SIGTERM)
    except (OSError, ProcessLookupError):
        pass


class ResourceMonitor(threading.Thread):
    """Samples the summed resident memory of the build's process group (Linux)."""

    def __init__(self, sid):
        super().__init__(daemon=True)
        self.sid, self.peak, self.stop_flag = sid, 0, threading.Event()

    def run(self):
        if IS_WIN:
            return
        page = os.sysconf("SC_PAGE_SIZE")
        while not self.stop_flag.wait(2.0):
            total = 0
            for d in os.listdir("/proc"):
                if not d.isdigit():
                    continue
                try:
                    with open(f"/proc/{d}/stat") as f:
                        rest = f.read().rsplit(")", 1)[1].split()
                    if int(rest[3]) != self.sid:  # session id
                        continue
                    with open(f"/proc/{d}/statm") as f:
                        total += int(f.read().split()[1]) * page
                except (OSError, IndexError, ValueError):
                    continue
            self.peak = max(self.peak, total)


def run(cmd, cwd, env, rep: Reporter, on_line=None, low_priority=False, monitor=None):
    """Run a command, streaming its output to rep.log; raises BuildError on failure and
    Cancelled when the player cancels."""
    rep.log("$ " + " ".join(str(c) for c in cmd))
    kw = {}
    if IS_WIN:
        kw["creationflags"] = 0x08000000 | 0x00000200 | (0x00004000 if low_priority else 0)
    else:
        kw["start_new_session"] = True
        if low_priority:
            kw["preexec_fn"] = lambda: os.nice(10)
    try:
        proc = subprocess.Popen([str(c) for c in cmd], cwd=str(cwd), env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True,
                                encoding="utf-8", errors="replace", bufsize=1, **kw)
    except OSError as e:
        raise BuildError(f"Could not start {cmd[0]}: {e}")
    mon = None
    if monitor is not None and not IS_WIN:
        mon = ResourceMonitor(proc.pid)
        mon.start()
    stop = threading.Event()

    def watch():
        while not stop.wait(0.5):
            if rep.cancelled():
                kill_tree(proc)
                return
    threading.Thread(target=watch, daemon=True).start()
    try:
        for line in proc.stdout:
            line = line.rstrip("\n")
            if on_line:
                on_line(line)
            rep.log(line)
        code = proc.wait()
    finally:
        stop.set()
        if mon:
            mon.stop_flag.set()
            monitor["peak"] = max(monitor.get("peak", 0), mon.peak)
    if rep.cancelled():
        raise Cancelled()
    if code != 0:
        raise BuildError(f"{Path(str(cmd[0])).name} failed (exit code {code}); see the log above.")


# ---- the steps ----------------------------------------------------------------------------

def _dl_progress(rep, lo, hi, what):
    def cb(done, total):
        if total:
            rep.progress(lo + (hi - lo) * done / total, f"{what}: {done / 1e6:.0f} of {total / 1e6:.0f} MB")
    return cb


LLVM_BIN_KEEP = {
    "clang", "clang++", "clang-cl", "clang-cpp", "clang-scan-deps", "lld", "ld.lld", "ld64.lld", "lld-link",
    "llvm-ar", "llvm-ranlib", "llvm-nm", "llvm-objcopy", "llvm-strip", "llvm-objdump", "llvm-readelf",
    "llvm-readobj", "llvm-lib", "llvm-rc", "llvm-cvtres", "llvm-mt", "llvm-dlltool", "llvm-symbolizer",
}


def llvm_member_wanted(name: str) -> bool:
    """The full LLVM release is 12 GB unpacked; the build needs the compiler driver, the linker,
    the archiver tools and the compiler's own headers (lib/clang)."""
    parts = Path(name).parts
    if parts[0] == "bin" and len(parts) == 2:
        base = parts[1][:-4] if parts[1].endswith(".exe") else parts[1]
        return base in LLVM_BIN_KEEP or re.fullmatch(r"clang-\d+", base) is not None
    return parts[0] in ("lib",) and len(parts) > 1 and parts[1] == "clang" or parts[0].upper().startswith("LICENSE")


def ensure_toolchain(opts: Options, rep: Reporter, lo, hi):
    tools = build_env.detect_tools(opts.root, opts.prefer_downloaded, max_age=0)
    for p in tools.problems:
        raise BuildError(p)
    if not tools.missing:
        for k, v in tools.source.items():
            rep.log(f"{k}: {v}")
        return tools
    cache = opts.root / "downloads"
    tdir = build_env.toolchain_dir(opts.root)
    n = len(tools.missing)
    for i, name in enumerate(tools.missing):
        a, b = lo + (hi - lo) * i / n, lo + (hi - lo) * (i + 1) / n
        mb = pins.SIZES_MB.get(name, 0)
        rep.status(f"Downloading {name} ({mb} MB)")
        rep.log(f"download {pins.DOWNLOADS[name][0]}")
        arc = fetch.download(name, cache, _dl_progress(rep, a, a + (b - a) * 0.6, name), rep.cancelled)
        rep.status(f"Unpacking {name}")
        if name.startswith("llvm"):
            dest = tdir / f"llvm-{pins.LLVM_VERSION}"
            fetch.unpack(name, arc, dest, strip=1, only=llvm_member_wanted,
                         progress=_dl_progress(rep, a + (b - a) * 0.6, b, "unpack"), cancelled=rep.cancelled)
            arc.unlink(missing_ok=True)  # 2 GB: not needed any more
        elif name.startswith("cmake"):
            fetch.unpack(name, arc, tdir / f"cmake-{pins.CMAKE_VERSION}", strip=1, cancelled=rep.cancelled)
        elif name.startswith("ninja"):
            fetch.unpack(name, arc, tdir / f"ninja-{pins.NINJA_VERSION}", cancelled=rep.cancelled)
            if not IS_WIN:
                os.chmod(tdir / f"ninja-{pins.NINJA_VERSION}" / "ninja", 0o755)
        rep.progress(b)
    tools = build_env.detect_tools(opts.root, opts.prefer_downloaded, max_age=0)
    for p in tools.problems:
        raise BuildError(p)
    if tools.missing:
        raise BuildError("The toolchain is incomplete after the download: " + ", ".join(tools.missing))
    for k, v in tools.source.items():
        rep.log(f"{k}: {v}")
    return tools


def ensure_sdk(opts: Options, rep: Reporter, lo, hi) -> Path:
    """The patched SDK prefix: the pinned nightly plus this project's patched plugin, runtime and
    codegen tool (Linux), or the project's own SDK build (Windows). Returns the prefix."""
    prefix = sdk_dir(opts.root)
    marker = prefix / ".forza-sdk-ready"
    overlay = sdk_overlay_archive()
    dev = dev_sdk_overlay()
    ident = f"{pins.SDK_TAG}|{overlay.stat().st_size if overlay else (str(dev) if dev else 'none')}"
    if marker.is_file() and marker.read_text() == ident:
        rep.log(f"SDK ready: {prefix}")
        return prefix
    if overlay is None and dev is None:
        raise BuildError("The patched SDK files (sdk-overlay/rexglue-overlay-%s-amd64.tar.gz) are "
                         "not installed next to the launcher. Reinstall the launcher." % PLAT)
    shutil.rmtree(prefix, ignore_errors=True)
    stage = prefix.with_name(prefix.name + ".part")
    shutil.rmtree(stage, ignore_errors=True)
    if IS_WIN:
        rep.status("Unpacking the SDK")
        fetch.extract_tar(overlay, stage, cancelled=rep.cancelled)
    else:
        rep.status("Downloading the ReXGlue SDK (122 MB)")
        arc = fetch.download("sdk-linux", opts.root / "downloads", _dl_progress(rep, lo, lo + (hi - lo) * 0.6, "SDK"),
                             rep.cancelled)
        rep.status("Unpacking the SDK")
        fetch.extract_zip(arc, stage, strip=1, cancelled=rep.cancelled)
        if overlay is not None:
            fetch.extract_tar(overlay, stage, cancelled=rep.cancelled)
        else:  # development: the checkout's own overlay
            for rel in ("lib/librexgpu-xenos.so", "lib/librexruntime.so", "bin/rexglue"):
                dst = stage / rel
                dst.unlink(missing_ok=True)
                shutil.copy2((dev / rel).resolve(), dst)
        for rel in ("bin/rexglue", "lib/librexgpu-xenos.so", "lib/librexruntime.so"):
            if not (stage / rel).is_file():
                raise BuildError(f"The patched SDK is missing {rel}")
    os.replace(stage, prefix)
    marker.write_text(ident)
    rep.progress(hi)
    return prefix


def title_update_script() -> Path | None:
    for d in (paths.LAUNCHER_ROOT / "tools", paths.app_dir() / "project" / "tools", paths.REPO_ROOT / "tools"):
        if (d / "apply_title_update.py").is_file():
            return d / "apply_title_update.py"
    return None


def apply_title_update(opts: Options, rep: Reporter, game_files: Path) -> Path | None:
    """STUB until tools/apply_title_update.py lands (it is written by the DLC work, see
    docs/dlc.md): base default.xex + the player's title update package -> a patched
    default.xex that codegen then uses. Assumed interface:
        apply_title_update.py BASE_XEX TU_PACKAGE OUT_XEX
    Returns the patched xex, or None when no title update is used."""
    if not opts.title_update:
        return None
    script = title_update_script()
    if script is None:
        rep.log("WARNING: a title update was chosen, but this launcher has no tools/apply_title_update.py "
                "yet: building from the base default.xex without it.")
        rep.status("Title update step not available yet: using the base game")
        return None
    out = build_dir(opts.root) / "tu" / "default.xex"
    out.parent.mkdir(parents=True, exist_ok=True)
    rep.status("Applying your title update")
    cmd = [sys.executable] + (["--run-script"] if paths.FROZEN else []) + [
        script, game_files / "default.xex", opts.title_update, out]
    run(cmd, opts.root, os.environ.copy(), rep, low_priority=opts.low_priority)
    if not out.is_file():
        raise BuildError("The title update step did not produce a patched default.xex.")
    return out


def sync_sources(opts: Options, rep: Reporter, game_files: Path, xex: Path | None = None) -> Path:
    proj = project_dir()
    if proj is None:
        raise BuildError("The project sources (project/forza-recomp) are not installed next to the launcher.")
    dest = build_dir(opts.root) / "forza-recomp"
    dest.mkdir(parents=True, exist_ok=True)
    changed = 0
    wanted = set()
    for rel in source_files(proj):
        src, dst = proj / "forza-recomp" / rel, dest / rel
        wanted.add(rel)
        data = src.read_bytes()
        if rel.as_posix() == "forza_manifest.toml":
            if xex is not None:  # the title-updated executables replace the base ones
                for name in ("default.xex", "XMediaFacade_default.xex", "SpeechFacade_default.xex"):
                    patched = xex.parent / name
                    if patched.is_file():
                        data = data.replace(f"../extracted/game/{name}".encode(), patched.resolve().as_posix().encode())
            data = data.replace(b"../extracted/game", game_files.resolve().as_posix().encode())
        if not dst.is_file() or dst.read_bytes() != data:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)  # unchanged files keep their time, so ninja rebuilds only changes
            changed += 1
    rep.log(f"project sources: {len(wanted)} files, {changed} updated")
    return dest


def msvc_environment(opts: Options, rep: Reporter, tools, lo, hi):
    """Windows: the environment (INCLUDE, LIB, PATH) of the MSVC CRT and Windows SDK, from an
    installed Visual Studio, else from xwin, which needs the player to accept Microsoft's license."""
    pf = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                              "installationPath"], capture_output=True, text=True).stdout.strip()
        vcvars = Path(out) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat" if out else None
        if vcvars and vcvars.is_file():
            res = subprocess.run(f'cmd /c ""{vcvars}" >nul && set"', capture_output=True, text=True, shell=True)
            env = {}
            for line in res.stdout.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    env[k] = v
            if env.get("INCLUDE") and env.get("LIB"):
                rep.log(f"MSVC: Visual Studio at {out}")
                return env
    xdir = opts.root / "xwin"
    if not (xdir / "crt" / "include").is_dir():
        if not opts.accept_ms_license:
            raise BuildError("The Microsoft C++ runtime and Windows SDK are needed and are not installed. "
                             "Accept Microsoft's license in the setup page to download them, or install "
                             "Visual Studio Build Tools with the C++ and Windows SDK workloads.")
        arc = fetch.download("xwin-windows", opts.root / "downloads", cancelled=rep.cancelled)
        tooldir = build_env.toolchain_dir(opts.root) / f"xwin-{pins.XWIN_VERSION}"
        fetch.unpack("xwin-windows", arc, tooldir, strip=1)
        exe = next(tooldir.rglob("xwin.exe"))
        rep.status("Downloading the Microsoft C++ runtime and Windows SDK")
        run([exe, "--accept-license", "--cache-dir", opts.root / "downloads" / "xwin-cache", "splat",
             "--output", xdir], opts.root, os.environ.copy(), rep)
    inc = [xdir / "crt" / "include", xdir / "sdk" / "include" / "ucrt", xdir / "sdk" / "include" / "um",
           xdir / "sdk" / "include" / "shared", xdir / "sdk" / "include" / "winrt"]
    lib = [xdir / "crt" / "lib" / "x86_64", xdir / "sdk" / "lib" / "um" / "x86_64",
           xdir / "sdk" / "lib" / "ucrt" / "x86_64"]
    env = dict(os.environ)
    env["INCLUDE"] = os.pathsep.join(map(str, inc))
    env["LIB"] = os.pathsep.join(map(str, lib))
    return env


NINJA_RE = re.compile(r"^\[(\d+)/(\d+)\]")


def run_build(opts: Options, rep: Reporter, game_files: Path) -> Path:
    """The whole pipeline; returns the runtime directory. Safe to cancel and run again."""
    t0 = time.time()
    root = opts.root
    root.mkdir(parents=True, exist_ok=True)
    if not (game_files / "default.xex").is_file():
        raise BuildError(f"No default.xex in {game_files}: extract your game files first.")
    cid = current_build_id(root, opts.title_update)
    state = read_state(root)
    if opts.clean:
        shutil.rmtree(build_dir(root), ignore_errors=True)
        state = {}
    write_state(root, {"started": time.time(), "id": cid, "complete": False})
    monitor = {}

    rep.status("Checking the toolchain")
    tools = ensure_toolchain(opts, rep, 0.0, 0.14)
    rep.status("Preparing the SDK")
    sdk = ensure_sdk(opts, rep, 0.14, 0.17)
    patched = apply_title_update(opts, rep, game_files)
    rep.status("Preparing the project sources")
    src = sync_sources(opts, rep, game_files, patched)
    env = build_env.tool_env(tools)
    cmake_extra = []
    if IS_WIN:
        env.update(msvc_environment(opts, rep, tools, 0.17, 0.18))
        llvm_bin = str(Path(tools.cc).parent)
        env["PATH"] = os.pathsep.join([llvm_bin] + [p for p in tools.path_dirs if p != llvm_bin] +
                                      [env.get("PATH", "")])
        env["FORZA_LLVM_BIN"] = llvm_bin
        cmake_extra += ["-DCMAKE_TOOLCHAIN_FILE=" + str((project_dir() / "tools/cmake/windows-native-clang-cl.cmake").as_posix())]
    else:
        cmake_extra += ["-DCMAKE_CXX_COMPILER=" + tools.cxx]
    rexglue = sdk / "bin" / ("rexglue" + EXE)

    # Codegen: recompile the player's default.xex (and its two DLLs) to C++.
    rep.status("Generating code from your default.xex (a minute or two)")
    rep.progress(0.18, "codegen")
    run([rexglue, "codegen", "forza_manifest.toml"], src, env, rep, low_priority=opts.low_priority)
    rep.progress(0.22)

    bdir = src / "out" / "build" / PLAT
    rep.status("Configuring")
    run([tools.cmake, "-S", src, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_PREFIX_PATH={sdk.as_posix()}", f"-Drexglue_DIR={(sdk / 'lib/cmake/rexglue').as_posix()}",
         f"-DFORZA_REXGLUE={rexglue.as_posix()}",
         f"-DCMAKE_MAKE_PROGRAM={Path(tools.ninja).as_posix()}"] +
        ([f"-DFORZA_XENOS_PLUGIN={(sdk / 'lib/librexgpu-xenos.so').as_posix()}"] if not IS_WIN else []) +
        cmake_extra, src, env, rep, low_priority=opts.low_priority)
    rep.progress(0.24)

    # Compile. Codegen may rewrite the DLL modules' sources during the first pass, which ninja only
    # sees on its next run, so it runs twice (the second pass is a no-op when nothing changed).
    for attempt in (1, 2):
        rep.status("Compiling" + (" (second pass)" if attempt == 2 else ": this takes a long time"))
        last = {"n": 0}

        def on_line(line):
            m = NINJA_RE.match(line)
            if m:
                n, total = int(m.group(1)), int(m.group(2))
                last["n"] = n
                if total:
                    rep.progress(0.24 + 0.70 * n / total, f"compiling {n} of {total}")
        run([tools.ninja, "-C", bdir, f"-j{opts.jobs}"], src, env, rep, on_line=on_line,
            low_priority=opts.low_priority, monitor=monitor)
    rep.progress(0.95)

    rep.status("Installing")
    rt = stage_runtime(opts, rep, bdir, sdk)
    update = build_dir(root) / "tu" / "update"
    if patched is not None and update.is_dir():   # the title update's other files (media.zip)
        shutil.rmtree(rt / "update", ignore_errors=True)
        shutil.copytree(update, rt / "update")
    wall = time.time() - t0
    peak = monitor.get("peak", 0)
    write_state(root, {"started": t0, "id": cid, "complete": True,
                       "when": time.strftime("%Y-%m-%d %H:%M"), "seconds": round(wall),
                       "peak_rss_gb": round(peak / 2 ** 30, 1), "jobs": opts.jobs})
    (rt / "version.txt").write_text(f"launcher {__version__}, built {time.strftime('%Y-%m-%d')} "
                                    f"(sources {sources_hash(project_dir())[:8]})\n")
    rep.progress(1.0, "done")
    rep.log(f"Build finished in {wall / 60:.1f} min, jobs {opts.jobs}, "
            f"peak build memory {peak / 2 ** 30:.1f} GB")
    return rt


def stage_runtime(opts: Options, rep: Reporter, bdir: Path, sdk: Path) -> Path:
    """Copy what the game needs to run into runtime/ so the build tree can be deleted."""
    rt = runtime_dir(opts.root)
    rt.mkdir(parents=True, exist_ok=True)
    if IS_WIN:
        for f in [bdir / "forza.exe"] + sorted(bdir.glob("forza_*.dll")):
            shutil.copy2(f, rt / f.name)
        for dll in sorted((sdk / "bin").glob("*.dll")):
            if not dll.name.endswith(("d.dll", "rd.dll")):
                shutil.copy2(dll, rt / dll.name)
        return rt
    modules = ["forza_XMediaFacade_default", "forza_SpeechFacade_default"]
    for name in ["forza"] + modules:
        tmp = rt / ("." + name + ".new")
        shutil.copy2(bdir / name, tmp)
        os.replace(tmp, rt / name)
    for name in ("librexruntime.so", "librexgpu-xenos.so"):
        tmp = rt / ("." + name + ".new")
        shutil.copy2((sdk / "lib" / name).resolve(), tmp)
        os.replace(tmp, rt / name)
    # the plugin comes out of the SDK's build tree, whose runpath points into that tree
    script = rt / ".rpath.cmake"
    script.write_text(f'file(RPATH_SET FILE "{(rt / "librexgpu-xenos.so").as_posix()}" NEW_RPATH "$ORIGIN")\n')
    cmake = shutil.which("cmake") or str(build_env.downloaded_paths(opts.root)["cmake"])
    subprocess.run([cmake, "-P", str(script)], capture_output=True)
    script.unlink(missing_ok=True)
    return rt


def clean_build_tree(root: Path) -> int:
    """Delete the build tree (generated code, objects): the runtime keeps working. Returns bytes freed."""
    b = build_dir(root)
    size = sum(f.stat().st_size for f in b.rglob("*") if f.is_file()) if b.exists() else 0
    shutil.rmtree(b / "forza-recomp" / "out", ignore_errors=True)
    shutil.rmtree(b / "forza-recomp" / "generated", ignore_errors=True)
    return size
