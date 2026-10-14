"""Entry point: the window, or a few command-line modes.

  forza-launcher                 open the launcher
  forza-launcher --dry-run       print the forza.toml and command line Play would use (nothing is written)
  forza-launcher --live-server SCRIPT [args]   run the Forza Live server (the frozen app does this
                                 for the local server, so the player needs no Python)
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


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] == ["--live-server"]:
        run_live_server(argv[1:])
        return 0
    if argv[:1] == ["--version"]:
        print(__version__)
        return 0
    if argv[:1] == ["--dry-run"]:
        return dry_run()
    from .ui.window import run
    return run(argv)
