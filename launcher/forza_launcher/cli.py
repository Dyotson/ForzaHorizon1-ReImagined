"""Entry point: the window, or a few command-line modes.

  forza-launcher                 open the launcher
  forza-launcher --dry-run       print the forza.toml and command line Play would use (nothing is written)
  forza-launcher --live-server SCRIPT [args]   run the Forza Live server (the frozen app does this
                                 for the local server, so the player needs no Python)
  forza-launcher --run-script SCRIPT [args]   run a bundled Python tool (the frozen app has no Python)
  forza-launcher --build [--root DIR] [--jobs N] [--clean]   build the game without the window
  forza-launcher --version
"""
import runpy
import sys

from . import __version__


def run_live_server(argv):
    script = argv[0]
    sys.argv = [script] + argv[1:]
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(script)))
    runpy.run_path(script, run_name="__main__")


def dry_run():
    from . import game
    from .config import Config
    cfg = Config()
    install, looked = game.find_game(cfg)
    if install is None:
        print(game.missing_message(looked))
        return 1
    plan = game.build_plan(cfg, install, write=False)
    print(f"# {plan.toml_path}")
    print(plan.toml_text, end="")
    print("# environment")
    for k, v in sorted(plan.env.items()):
        print(f"{k}={v}")
    for k in plan.unset:
        print(f"unset {k}")
    print("# command (in %s)" % plan.cwd)
    print(plan.command_line())
    if plan.server_argv:
        print("# local server\n" + " ".join(plan.server_argv))
    for n in plan.notes:
        print("# note: " + n)
    return 0


def headless_build(argv):
    import argparse
    from pathlib import Path
    from . import builder
    from .config import Config
    ap = argparse.ArgumentParser(prog="forza-launcher --build")
    ap.add_argument("--root", help="data root (default: the launcher's)")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--accept-microsoft-license", action="store_true")
    ap.add_argument("--title-update", default="", help="your TU4 package")
    a = ap.parse_args(argv)
    cfg = Config()
    root = Path(a.root) if a.root else cfg.data_home()

    class Console(builder.Reporter):
        def log(self, text):
            print(text, flush=True)

        def status(self, text):
            print(f"== {text}", flush=True)

    opts = builder.Options(root, a.jobs, accept_ms_license=a.accept_microsoft_license, clean=a.clean,
                           title_update=a.title_update)
    try:
        rt = builder.run_build(opts, Console(), root / "game")
    except builder.BuildError as e:
        print(f"build failed: {e}", file=sys.stderr)
        return 1
    print(f"runtime: {rt}")
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] == ["--live-server"]:
        run_live_server(argv[1:])
        return 0
    if argv[:1] == ["--run-script"]:
        run_live_server(argv[1:])  # the same runner: a script with its folder on sys.path
        return 0
    if argv[:1] == ["--version"]:
        print(__version__)
        return 0
    if argv[:1] == ["--build"]:
        return headless_build(argv[1:])
    if argv[:1] == ["--dry-run"]:
        return dry_run()
    from .ui.window import run
    return run(argv)
