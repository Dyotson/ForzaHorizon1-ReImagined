"""Automatic installation for packaged Linux archives."""
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

from . import cleanenv, fetch, paths, update_helper, updater

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
                                       stdin=subprocess.DEVNULL, start_new_session=True, close_fds=True,
                                       env=cleanenv.clean_environment())
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
