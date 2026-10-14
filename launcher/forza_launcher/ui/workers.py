"""Background tasks: extracting, installing DLC, building. Each runs in a QThread and reports by signals."""
from pathlib import Path

from PySide6.QtCore import QObject, QThread, Signal

from .. import builder, fetch, installer, paths


class Task(QThread):
    log = Signal(str)
    status = Signal(str)
    progress = Signal(float, str)   # 0..1, detail
    done = Signal(bool, str)        # success, message

    def __init__(self, fn, name="task"):
        super().__init__()
        self.fn, self.name = fn, name
        self._cancel = False

    def cancel(self):
        self._cancel = True

    def cancelled(self):
        return self._cancel

    def run(self):
        try:
            msg = self.fn(self)
            self.done.emit(True, msg or "")
        except (installer.Cancelled, fetch.Cancelled):
            self.done.emit(False, "Cancelled.")
        except (installer.InstallError, builder.BuildError) as e:
            log = paths.log_error(f"{self.name}: {e}")
            self.done.emit(False, f"{e}\n(Details: {log})")
        except BaseException as e:  # never let a thread die silently (SystemExit from a tool included)
            log = paths.log_error(f"{self.name}: unexpected {type(e).__name__}", e)
            self.done.emit(False, f"Unexpected error: {type(e).__name__}: {e}\n(Details: {log})")


class TaskReporter(builder.Reporter):
    """Adapts a Task to the builder's Reporter."""

    def __init__(self, task: Task):
        self.t = task

    def log(self, text):
        self.t.log.emit(text)

    def status(self, text):
        self.t.status.emit(text)

    def progress(self, fraction, detail=""):
        self.t.progress.emit(float(fraction), detail)

    def cancelled(self):
        return self.t.cancelled()


def extract_task(package, dest):
    def fn(t: Task):
        def prog(done, total, name):
            t.progress.emit(done / total if total else 0, name)
        t.status.emit("Extracting the game files")
        notes = []
        total = installer.extract_game(package, Path(dest), prog, t.cancelled, notes.append)
        return f"Extracted {total / 2**30:.1f} GB. " + " ".join(notes)
    return Task(fn, "extract game files")


def verify_task(folder):
    """Check an extracted game folder (decoding the executable of another printing takes a while)."""
    def fn(t: Task):
        t.status.emit("Checking the game files")
        ok, msg = installer.check_game_folder(Path(folder))
        if not ok:
            raise installer.InstallError(msg)
        return msg
    return Task(fn, "check game files")


def dlc_task(folder, user_data):
    def fn(t: Task):
        def prog(done, total, name):
            t.progress.emit(done / total if total else 0, name)
        t.status.emit("Installing DLC")
        n = installer.install_dlc_folder(folder, Path(user_data), prog, t.cancelled, t.log.emit)
        return f"Installed {n} DLC package(s)."
    return Task(fn, "install DLC")


def build_task(opts, game_files):
    def fn(t: Task):
        rt = builder.run_build(opts, TaskReporter(t), Path(game_files))
        return f"Built: {rt}"
    return Task(fn, "build")
