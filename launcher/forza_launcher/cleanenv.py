"""The environment for every external program the launcher starts (rexglue, cmake, ninja, clang,
git, the game, the live server, xdg-open ...).

A frozen launcher (PyInstaller) points LD_LIBRARY_PATH at its own _internal folder, whose libstdc++
and friends are the build machine's and are often older than the system's: a child started with
that path fails with "libstdc++.so.6: version GLIBCXX_3.4.31 not found" (issue #87). The bootloader
keeps the player's own value in LD_LIBRARY_PATH_ORIG; this puts it back, and drops the other
variables the bundle sets for itself.
"""
import os
import sys

# Library search paths: the bootloader saves the original in <NAME>_ORIG (set only when there was one).
LIBRARY_PATHS = ("LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH")
# Path lists a bundle may add its own folders to (they point into _internal, never at the player's tools).
BUNDLE_PATHS = ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                "PYTHONHOME", "PYTHONPATH", "SSL_CERT_FILE", "SSL_CERT_DIR", "REQUESTS_CA_BUNDLE")
# The bootloader's own markers (a child that inherits them thinks it is part of this process).
BOOTLOADER_VARS = ("_MEIPASS2", "_PYI_APPLICATION_HOME_DIR", "_PYI_ARCHIVE_FILE", "_PYI_PARENT_PROCESS_LEVEL",
                   "_PYI_LINUX_PROCESS_NAME", "PYINSTALLER_RESET_ENVIRONMENT")


def frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


def bundle_dir() -> str:
    return os.path.normpath(getattr(sys, "_MEIPASS", "") or "") if getattr(sys, "_MEIPASS", "") else ""


def _inside(entry: str, folder: str) -> bool:
    if not folder or not entry:
        return False
    e = os.path.normpath(entry)
    return e == folder or e.startswith(folder + os.sep)


def _strip_bundle(value: str, folder: str) -> str:
    return os.pathsep.join(p for p in value.split(os.pathsep) if p and not _inside(p, folder))


def clean_environment(base=None) -> dict:
    """A copy of `base` (default os.environ) as the player's shell had it, without the frozen
    launcher's library path and bundle variables. Outside a frozen launcher it is just a copy."""
    env = dict(os.environ if base is None else base)
    folder = bundle_dir()
    for name in LIBRARY_PATHS:
        saved = env.pop(name + "_ORIG", None)
        if saved is not None:
            # the bootloader's saved value (empty: the variable was not set before)
            if saved:
                env[name] = saved
            else:
                env.pop(name, None)
        elif frozen():
            # no saved value: nothing but the bundle's own folders can be in it
            kept = _strip_bundle(env.get(name, ""), folder) if folder else ""
            if kept:
                env[name] = kept
            else:
                env.pop(name, None)
    if frozen():
        for name in BUNDLE_PATHS:
            if name in env:
                kept = _strip_bundle(env[name], folder) if folder else ""
                if kept:
                    env[name] = kept
                else:
                    env.pop(name, None)
        for name in BOOTLOADER_VARS:
            env.pop(name, None)
    return env


def restore_process_environment() -> None:
    """Put the player's library path back in os.environ itself. The loader read LD_LIBRARY_PATH when
    this process started, so what is already loaded and what Qt loads later are not affected, but
    everything that starts programs on our behalf (Qt's xdg-open, ...) stops inheriting _internal."""
    if sys.platform.startswith("win"):
        return
    clean = clean_environment()
    for name in LIBRARY_PATHS:
        if name in clean:
            os.environ[name] = clean[name]
        else:
            os.environ.pop(name, None)
        os.environ.pop(name + "_ORIG", None)
