"""Standalone archive update helper. All imports happen before moving the running bundle.

The frozen launcher runs this source file via --run-script, outside the Qt process.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def clean_environment():
    """Undo PyInstaller's loader path before running another executable."""
    env = os.environ.copy()
    original = env.pop("LD_LIBRARY_PATH_ORIG", None)
    if original is None:
        env.pop("LD_LIBRARY_PATH", None)
    else:
        env["LD_LIBRARY_PATH"] = original
    return env


def write_result(path, message):
    path = Path(path)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(message + "\n", encoding="utf-8")
    temporary.replace(path)


def check_executable(directory, expected_version=None):
    executable = Path(directory) / "forza-launcher"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise RuntimeError("The update has no executable launcher.")
    result = subprocess.run([str(executable), "--version"], cwd=directory, env=clean_environment(),
                            capture_output=True, text=True, timeout=30)
    if result.returncode or (expected_version is not None and result.stdout.strip() != expected_version):
        raise RuntimeError("The updated launcher failed its startup check: " +
                           (result.stderr.strip() or result.stdout.strip())[:1000])


def restart(directory):
    os.chdir(directory)
    executable = str(Path(directory) / "forza-launcher")
    os.execve(executable, [executable], clean_environment())


def apply_archive(job, wait_seconds=60):
    """Wait for Qt to exit, rename on the same filesystem, roll back failed replacement."""
    import fcntl  # Linux only; the rest of the launcher also imports this module on Windows.
    destination, staged, backup = (Path(job[k]) for k in ("destination", "staged", "backup"))
    result = Path(job["result"])
    swapped = False
    moved_old = False
    # flock is released by the kernel if this helper is interrupted. Staging is private per job.
    with open(destination.parent / ".forza-updater.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fingerprint = destination.stat()
        if [fingerprint.st_dev, fingerprint.st_ino] != job["fingerprint"]:
            raise RuntimeError("Another update changed this installation. Check for updates again.")
        check_executable(staged, job["version"])
        Path(job["ready"]).touch()
        deadline = time.monotonic() + wait_seconds
        while True:
            try:
                os.kill(job["parent_pid"], 0)
            except ProcessLookupError:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("The launcher did not close. The installed copy was left unchanged.")
            time.sleep(.1)
        try:
            destination.rename(backup)
            moved_old = True
            staged.rename(destination)
            swapped = True
            check_executable(destination, job["version"])
            write_result(result, "success: Launcher updated to " + job["version"] + ". Previous copy: " + str(backup))
            restart(destination)
        except Exception as exc:
            if swapped:
                destination.rename(staged)
            if moved_old:
                backup.rename(destination)
            recovery = "The previous launcher was restored." if moved_old else "The installed launcher was kept."
            write_result(result, "failed: " + str(exc) + " " + recovery + " Log: " + job["log"])
            restart(destination)
            raise


def main():
    job = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    try:
        apply_archive(job)
    except Exception as exc:
        # Do not replace the more specific rollback result written by apply_archive.
        result = Path(job["result"])
        if not result.exists():
            write_result(result, "failed: " + str(exc) + " Log: " + job["log"])
        raise


if __name__ == "__main__":
    main()
