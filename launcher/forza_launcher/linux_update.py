"""Automatic installation for Linux archives and Flatpak bundles."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid

from . import fetch, paths, update_helper, updater

APP_ID = "io.github.dyotson.ForzaHorizon1ReImagined"
FLATPAK_RESTART_SCRIPT = '''nohup flatpak run --instance-id-fd=3 "$1" "--env=FORZA_UPDATE_READY=$4" "$2" >>"$3" 2>&1 </dev/null 3>"$4.instance" &
child=$!
attempt=0
while [ "$attempt" -lt 60 ]; do
    if [ -f "$4" ]; then
        rm -f "$4.instance"
        exit 0
    fi
    if ! kill -0 "$child" 2>/dev/null; then
        wait "$child"
        rm -f "$4.instance"
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 1
done
instance=$(cat "$4.instance")
if [ -n "$instance" ]; then
    flatpak kill "$instance" >>"$3" 2>&1
fi
kill -KILL "$child" 2>/dev/null
wait "$child" 2>/dev/null
rm -f "$4.instance"
exit 1
'''


def verify_download(archive, asset, cancelled=lambda: False):
    digest = hashlib.sha256()
    size = 0
    with Path(archive).open("rb") as stream:
        while chunk := stream.read(1 << 20):
            if cancelled():
                raise fetch.Cancelled()
            digest.update(chunk)
            size += len(chunk)
    if size != asset.size or digest.hexdigest() != asset.digest:
        raise updater.UpdateError("The downloaded update changed. Check for updates and download it again.")


def result_path():
    return paths.config_dir() / "update-result.txt"


def consume_result():
    path = result_path()
    try:
        message = path.read_text(encoding="utf-8")[:4000].strip()
        path.unlink()
        return message
    except OSError:
        return ""


def acknowledge_gui_ready():
    """Called only once the replacement Qt window has been shown."""
    value = os.environ.pop("FORZA_UPDATE_READY", "")
    if not value:
        return False
    ready = Path(value)
    if ready.parent.resolve() != paths.config_dir().resolve() or not ready.name.startswith("update-ready-"):
        return False
    try:
        ready.touch()
        return True
    except OSError as exc:
        paths.log_error("acknowledge updated launcher", exc)
        return False


def prepare_archive(archive, asset, version, destination, config_dir, protected=(), cancelled=lambda: False):
    """Extract into a private sibling; do not touch the installed copy during preparation."""
    destination = Path(destination).resolve()
    config_dir = Path(config_dir).resolve()
    if not (destination / "forza-launcher").is_file():
        raise updater.UpdateError("Cannot find the installed launcher. Install a packaged release first.")
    for path in (*protected, config_dir):
        if Path(path).resolve().is_relative_to(destination):
            raise updater.UpdateError("Game files or user data are inside the launcher folder. Move them to a "
                                     "separate data folder in Setup before updating.")
    verify_download(archive, asset, cancelled)
    config_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".forza-update-", dir=destination.parent))
    try:
        with tarfile.open(archive, "r:gz") as bundle:
            members = bundle.getmembers()
            root = "ForzaHorizon1-ReImagined"
            if not members or any(m.name.split("/")[0] != root for m in members):
                raise updater.UpdateError("The update archive has an unexpected layout.")
            needed = sum(m.size for m in members if m.isfile())
            if shutil.disk_usage(work).free < needed + 16 * 2**20:
                raise updater.UpdateError("There isn't enough free space to stage the launcher update.")
            for member in members:
                if cancelled():
                    raise fetch.Cancelled()
                bundle.extract(member, work, filter="data")
        staged = work / root
        update_helper.check_executable(staged, version)
        # Plain Python helpers must also work after the bundle directory has moved.
        helper = work / "update_helper.py"
        shutil.copy2(Path(update_helper.__file__), helper)
        info = destination.stat()
        job = {"destination": str(destination), "staged": str(staged), "backup": str(work / "previous"),
               "fingerprint": [info.st_dev, info.st_ino], "version": version,
               "parent_pid": os.getpid(), "result": str(config_dir / "update-result.txt"),
               "log": str(config_dir / "update-install.log"), "ready": str(work / "ready")}
        job_file = work / "job.json"
        job_file.write_text(json.dumps(job), encoding="utf-8")
        return helper, job_file, job
    except BaseException:
        shutil.rmtree(work, ignore_errors=True)
        raise


def start_archive(prepared, cancelled=lambda: False):
    helper, job_file, job = prepared
    if cancelled():
        shutil.rmtree(helper.parent, ignore_errors=True)
        raise fetch.Cancelled()
    result = Path(job["result"])
    result.unlink(missing_ok=True)
    argv = ([sys.executable, "--run-script", str(helper), str(job_file)] if paths.FROZEN else
            [sys.executable, str(helper), str(job_file)])
    process = None
    try:
        with open(job["log"], "a") as log:
            process = subprocess.Popen(argv, cwd=helper.parent, stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, start_new_session=True, close_fds=True)
        deadline = time.monotonic() + 15
        while not Path(job["ready"]).exists():
            if process.poll() is not None:
                raise updater.UpdateError("The update helper could not start. See " + job["log"])
            if time.monotonic() >= deadline or cancelled():
                raise updater.UpdateError("The update helper did not become ready. Try again. Log: " + job["log"])
            time.sleep(.05)
        return process
    except BaseException:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
        shutil.rmtree(helper.parent, ignore_errors=True)
        raise


def install_archive(archive, asset, tag, destination, config_dir, protected=(), cancelled=lambda: False):
    prepared = prepare_archive(archive, asset, tag.removeprefix("v"), destination, config_dir, protected, cancelled)
    return start_archive(prepared, cancelled)


def host_run(arguments, timeout=30, watch_bus=True, environment=None):
    try:
        flags = ["--watch-bus"] if watch_bus else []
        flags += [f"--env={key}={value}" for key, value in (environment or {}).items()]
        result = subprocess.run(["flatpak-spawn", "--host", *flags, *arguments], capture_output=True,
                                text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise updater.UpdateError("Could not reach the Flatpak installer on the host. "
                                 "The installed launcher was kept. " + str(exc)) from exc
    if result.returncode:
        raise updater.UpdateError("Flatpak could not install the update. The existing launcher is still available. "
                                 + (result.stderr.strip() or result.stdout.strip())[-2000:])
    return result.stdout.strip()


def install_flatpak(archive, asset, info_path=Path("/.flatpak-info")):
    """Flatpak deploys atomically while the current sandbox continues using its old deployment."""
    import configparser
    verify_download(archive, asset)
    info = configparser.ConfigParser()
    info.read(info_path)
    location = info.get("Instance", "app-path", fallback="")
    if info.get("Application", "name", fallback="") != APP_ID or not location:
        raise updater.UpdateError("Could not identify this Flatpak installation. The existing launcher was kept.")
    scope = None
    for option in ("--user", "--system"):
        try:
            installed = host_run(["flatpak", "info", option, "--show-location", APP_ID])
        except updater.UpdateError:
            continue
        running = Path(location.rstrip("/").removesuffix("/files"))
        active = Path(installed.rstrip("/").removesuffix("/files"))
        # A failed restart leaves this sandbox on the previous commit. Its installation
        # and branch still match even though the currently deployed checksum changed.
        if installed and (running == active or running.parent == active.parent):
            scope = option
            break
    if scope is None:
        raise updater.UpdateError("Could not access this Flatpak installation. Automatic updates need host "
                                 "permission (org.freedesktop.Flatpak) and a user or default system installation.")
    directory = result_path().parent
    directory.mkdir(parents=True, exist_ok=True)
    log_path = directory / "update-install.log"
    ready = directory / ("update-ready-" + uuid.uuid4().hex)
    result_path().unlink(missing_ok=True)
    try:
        try:
            output = host_run(["flatpak", "install", scope, "--bundle", "--or-update", "--noninteractive",
                               "--assumeyes", str(Path(archive).resolve())], timeout=1800, environment={"LC_ALL": "C"})
        except updater.UpdateError as exc:
            # Bundle installs reject an identical deployed commit, even with --or-update
            # or --reinstall. Treat only this exact target's native no-op as a restart retry.
            if not str(exc).endswith(APP_ID + " already installed"):
                raise
            output = "The same bundle is already installed; retrying launcher startup.\n" + str(exc)
        log_path.write_text(output + "\n", encoding="utf-8")
        # The host session helper may have been activated without this window's display.
        # Never forward sandbox-only bus, library or XDG directory paths to the host.
        display = {key: os.environ[key] for key in ("DISPLAY", "WAYLAND_DISPLAY") if os.environ.get(key)}
        try:
            host_run(["/bin/sh", "-c", FLATPAK_RESTART_SCRIPT, "forza-update-restart", scope, APP_ID,
                      str(log_path), str(ready)], timeout=180, watch_bus=False, environment=display)
        except updater.UpdateError as exc:
            raise updater.UpdateError("Update installed, but the new launcher could not restart. "
                                     "This window remains open. Retry or open the launcher from your "
                                     "application menu. Log: " + str(log_path)) from exc
        update_helper.write_result(result_path(), "success: Launcher updated. Flatpak installed the verified bundle.")
    except updater.UpdateError as exc:
        with log_path.open("a", encoding="utf-8") as log:
            log.write(str(exc) + "\n")
        raise
    finally:
        ready.unlink(missing_ok=True)
