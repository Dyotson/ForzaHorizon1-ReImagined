"""Finding the game binary and starting it the way tools/play.sh and play.bat do."""
import glob
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import TITLE_ID, paths
from .config import Config, render_toml, write_toml

EXE = "forza.exe" if paths.IS_WINDOWS else "forza"
LOCAL_PROFILE = "B13EBABEBABEBABE"  # the runtime's own profile folder
LOCAL_SERVER = "127.0.0.1:21200"
LOCAL_HTTP_PORT = 21201


# --- locating the game ----------------------------------------------------------------

@dataclass
class GameInstall:
    dir: Path
    exe: Path
    lib_dirs: list = field(default_factory=list)  # added to LD_LIBRARY_PATH (Linux)
    source: str = ""


def _sdk_lib_dirs():
    """The SDK libraries of a source build, like play.sh (REXSDK, patched overlay, nightly)."""
    home = Path.home()
    for c in (os.environ.get("REXSDK"),
              home / "rexglue-sdk-patched-forza/0.10.0.24/linux-amd64",
              home / "rexglue-sdk-patched/0.10.0.24/linux-amd64",
              home / "rexglue-sdk-nightly/0.10.0.24/linux-amd64"):
        if c and (Path(c) / "lib").is_dir():
            return [Path(c) / "lib"]
    return []


def candidate_dirs(cfg: Config):
    override = cfg.advanced.get("game_dir", "").strip() or os.environ.get("FORZA_GAME_DIR", "")
    if override:
        yield Path(override), "set in Advanced settings", False
    from . import builder
    yield builder.runtime_dir(cfg.data_home()), "built by the launcher", False
    if paths.in_checkout() and not paths.IS_WINDOWS:  # development: this checkout's own build
        build = os.environ.get("FORZA_BUILD", "linux")
        yield paths.REPO_ROOT / "forza-recomp/out/build" / build, "the build in this checkout", True


def find_game(cfg: Config):
    """Returns (GameInstall | None, [places looked in])."""
    looked = []
    for d, why, needs_sdk in candidate_dirs(cfg):
        looked.append(f"{d} ({why})")
        exe = d / EXE
        if exe.is_file():
            return GameInstall(d, exe, _sdk_lib_dirs() if needs_sdk else [], why), looked
    return None, looked


def missing_message(looked):
    return ("The game has not been built on this computer yet. Open Setup and press Build.\n"
            "Looked in:\n  " + "\n  ".join(looked))


def game_version(game: GameInstall | None) -> str:
    """The build the package was made from: version.txt, else 'Built from commit X' in README.txt."""
    if game is None:
        return "not installed"
    for name in ("version.txt", "README.txt"):
        try:
            text = (game.dir / name).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if name == "version.txt" and text.strip():
            return text.strip().splitlines()[0]
        m = re.search(r"Built from commit (\w+)", text)
        if m:
            return "build " + m.group(1)
    if paths.in_checkout():
        try:
            out = subprocess.run(["git", "-C", str(paths.REPO_ROOT), "rev-parse", "--short", "HEAD"],
                                 capture_output=True, text=True, timeout=5)
            if out.returncode == 0 and out.stdout.strip():
                return "source build " + out.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return "unknown build"


def installed_dlc(cfg: Config) -> int:
    d = cfg.data_home() / "0000000000000000" / TITLE_ID / "00000002"
    try:
        return sum(1 for p in d.iterdir() if p.is_dir())
    except OSError:
        return 0


# --- the Forza Live server --------------------------------------------------------------

def server_script(game: GameInstall | None) -> Path | None:
    for d in ((game.dir,) if game else ()) + (paths.app_dir(), paths.REPO_ROOT):
        p = d / "server" / "forza_live_server.py"
        if p.is_file():
            return p
    return None


def server_listening(port=LOCAL_HTTP_PORT) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


def server_command(script: Path, data_dir: Path):
    args = ["--host", "127.0.0.1", "--port", LOCAL_SERVER.split(":")[1],
            "--http-port", str(LOCAL_HTTP_PORT), "--data-dir", str(data_dir)]
    if paths.FROZEN:  # run the server inside this executable (no Python needed)
        return [sys.executable, "--live-server", str(script)] + args
    return [sys.executable, str(script)] + args


# --- the Xenia save -----------------------------------------------------------------------

def default_xenia_content() -> Path:
    return Path(os.environ.get("FORZA_XENIA_CONTENT") or Path.home() / "xenia_canary_windows" / "content")


def xenia_candidates(content: Path):
    """XUIDs of Xenia profiles with a Forza Horizon save."""
    out = []
    for d in sorted(glob.glob(str(content / "*" / TITLE_ID / "00000001"))):
        xuid = Path(d).parent.parent.name
        if xuid != "0000000000000000":
            out.append(xuid)
    return out


def _link_dir(link: Path, target: Path):
    try:
        os.symlink(target, link, target_is_directory=True)
        return
    except OSError:
        pass
    if paths.IS_WINDOWS:  # a junction needs no privilege
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                       capture_output=True, creationflags=0x08000000)


def prepare_xenia(cfg: Config, refresh=False, log=print):
    """Copies the Xenia save (once; again with refresh) and returns (root, xuid)."""
    content = Path(cfg.profile.get("xenia_content") or default_xenia_content())
    xuid = cfg.profile.get("xenia_xuid", "").strip().upper()
    if not xuid:
        cands = xenia_candidates(content)
        if len(cands) != 1:
            raise RuntimeError(f"Found {len(cands)} Xenia profiles with a Forza Horizon save in "
                               f"{content}; choose one under Settings > Profile.")
        xuid = cands[0]
    if not re.fullmatch(r"[0-9A-F]{16}", xuid):
        raise RuntimeError("The Xenia XUID must be 16 hex digits.")
    src = content / xuid / TITLE_ID
    if not src.is_dir():
        raise RuntimeError(f"No Forza Horizon save at {src}")
    home = cfg.data_home()
    root = home / f"xenia-{xuid}"
    dest = root / LOCAL_PROFILE / TITLE_ID
    if refresh or not dest.is_dir():
        shutil.rmtree(dest, ignore_errors=True)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src, dest)
        log(f"Copied the Xenia save of {xuid} to {dest}")
    # Installed DLC lives with the normal profile; share it.
    (home / "0000000000000000").mkdir(parents=True, exist_ok=True)
    link = root / "0000000000000000"
    if not os.path.lexists(link):
        _link_dir(link, Path("..") / "0000000000000000" if not paths.IS_WINDOWS
                  else home / "0000000000000000")
    return root, xuid


# --- the launch plan -------------------------------------------------------------------------

@dataclass
class LaunchPlan:
    game: GameInstall
    cwd: Path
    argv: list
    env: dict            # variables to set
    unset: list          # variables to remove
    toml_path: Path
    toml_text: str
    server_argv: list | None = None   # a local Forza Live server to start (when not running)
    notes: list = field(default_factory=list)
    log_dir: Path | None = None

    def command_line(self) -> str:
        return " ".join(shlex.quote(a) for a in self.argv)


def split_args(text: str):
    text = text.strip()
    if not text:
        return []
    if paths.IS_WINDOWS:
        return [a.strip('"') for a in shlex.split(text, posix=False)]
    return shlex.split(text)


def parse_env(text: str):
    env = {}
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    return env


def build_plan(cfg: Config, game: GameInstall, write=True, refresh_xenia=False, log=print) -> LaunchPlan:
    home = cfg.data_home()
    data = cfg.game_files()
    toml_path = game.dir / "forza.toml"
    toml_text = write_toml(cfg, toml_path) if write else render_toml(cfg)

    args = [f"--game_data_root={data}"]
    notes = []
    env, unset = {}, []

    # The profile: your own, or a copy of your Xenia Canary save (play.sh --xenia).
    log_file = home / "forza.log"
    if cfg.profile["mode"] == "xenia":
        root, xuid = prepare_xenia(cfg, refresh_xenia, log) if write else (
            home / "xenia-<XUID>", cfg.profile.get("xenia_xuid") or "<XUID>")
        args += [f"--user_data_root={root}", f"--cache_root={home / 'cache'}",
                 f"--profile_xuid={xuid}"]
    elif cfg.advanced.get("data_root", "").strip():
        args.append(f"--user_data_root={home}")

    # Xbox LIVE: public/custom through the server named in forza.toml (and FORZA_SERVER, which
    # outranks it); local starts a Forza Live server on this PC; offline uses none.
    server_argv = None
    mode = cfg.online["mode"]
    if mode in ("public", "custom") and cfg.server_spec():
        env["FORZA_SERVER"] = cfg.server_spec()
    elif mode == "local":
        script = server_script(game)
        if script is None:
            notes.append("The Forza Live server files were not found: starting offline.")
            unset.append("FORZA_SERVER")
        else:
            env["FORZA_SERVER"] = LOCAL_SERVER
            if not server_listening():
                server_argv = server_command(script, home / "live_server")
    else:
        if mode == "custom":
            notes.append("No custom server address set: starting offline.")
        unset.append("FORZA_SERVER")

    if cfg.advanced.get("pad_per_user"):
        env["FORZA_PAD_PER_USER"] = "1"

    extra = split_args(cfg.advanced.get("extra_args", ""))
    if not any(a == "--log_file" or a.startswith("--log_file=") for a in extra):
        args.append(f"--log_file={log_file}")
    args += extra
    env.update(parse_env(cfg.advanced.get("extra_env", "")))

    if not paths.IS_WINDOWS:
        # play.sh: LD_LIBRARY_PATH="$SDK/lib:$BUILD" for a source build, "$DIR" for a package.
        libs = [str(p) for p in game.lib_dirs] + [str(game.dir)]
        if os.environ.get("LD_LIBRARY_PATH"):
            libs.append(os.environ["LD_LIBRARY_PATH"])
        env["LD_LIBRARY_PATH"] = ":".join(libs)
    return LaunchPlan(game, game.dir, [str(game.exe)] + args, env, unset, toml_path,
                      toml_text, server_argv, notes, home)


class Session:
    """A running game, and the local Forza Live server started for it."""

    def __init__(self, plan: LaunchPlan, log=print):
        self.plan = plan
        self.server = None
        self.proc = None
        self.log = log

    def start(self):
        plan = self.plan
        if plan.log_dir:
            plan.log_dir.mkdir(parents=True, exist_ok=True)
        flags = 0x08000000 if paths.IS_WINDOWS else 0  # CREATE_NO_WINDOW for the server
        if plan.server_argv:
            data_dir = Path(plan.server_argv[plan.server_argv.index("--data-dir") + 1])
            data_dir.mkdir(parents=True, exist_ok=True)
            self.server_log = open(data_dir / "server.log", "ab")
            self.server = subprocess.Popen(plan.server_argv, stdout=self.server_log,
                                           stderr=subprocess.STDOUT, creationflags=flags,
                                           stdin=subprocess.DEVNULL)
            self.log("Started a local Forza Live server")
        env = dict(os.environ)
        for k in plan.unset:
            env.pop(k, None)
        env.update(plan.env)
        self.proc = subprocess.Popen(plan.argv, cwd=plan.cwd, env=env, stdin=subprocess.DEVNULL)

    def poll(self):
        return None if self.proc is None else self.proc.poll()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()

    def cleanup(self):
        """After the game ends: stop our server and free guest memory left in /dev/shm."""
        if self.server and self.server.poll() is None:
            self.server.terminate()
        if not paths.IS_WINDOWS:
            clean_shm()


def clean_shm():
    """Guest memory (/dev/shm/xenia_memory_*, 5 GB) is not unlinked when the game is killed."""
    for shm in glob.glob("/dev/shm/xenia_memory_*"):
        try:
            if os.stat(shm).st_uid != os.getuid():
                continue
            name = os.path.basename(shm)
            in_use = False
            for maps in glob.glob("/proc/[0-9]*/maps"):
                try:
                    with open(maps, "r", errors="replace") as f:
                        if name in f.read():
                            in_use = True
                            break
                except OSError:
                    continue
            if not in_use:
                os.unlink(shm)
        except OSError:
            continue
