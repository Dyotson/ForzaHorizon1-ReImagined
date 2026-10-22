"""Prebuild shaders (#295): refused without a current build; only a log written by this run is quoted."""
import os
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
from forza_launcher import shader_prebuild  # noqa: E402


class Cfg:
    def data_home(self):
        return Path("/nonexistent")

    def __getitem__(self, key):
        return ""


class GuardTests(unittest.TestCase):
    def test_out_of_date_build_is_refused(self):
        for state in ("stale", "none", "partial", "nosource"):
            with mock.patch("forza_launcher.builder.build_status", return_value=(state, "x")):
                self.assertEqual(shader_prebuild.build_problem(Cfg()), shader_prebuild.REBUILD_FIRST)
                with mock.patch("forza_launcher.game.build_plan") as bp:
                    with self.assertRaises(shader_prebuild.PrebuildError) as cm:
                        shader_prebuild.prebuild(Cfg(), object())
                    bp.assert_not_called()
                self.assertEqual(str(cm.exception), "Rebuild the game first (Setup > Build): "
                                 "Prebuild shaders needs the current build.")

    def test_current_build_passes_and_status_shows_it(self):
        with mock.patch("forza_launcher.builder.build_status", return_value=("built", "x")):
            self.assertEqual(shader_prebuild.build_problem(Cfg()), "")
        self.assertEqual(shader_prebuild.status(None, "msg"), ("rebuild", "msg"))


FAKE = textwrap.dedent('''
    import sys
    MODE = %r
    if MODE == "flag":
        print("Unknown flag: --shader_prebuild_only"); sys.exit(2)
    if MODE == "log":
        log = [a.split("=", 1)[1] for a in sys.argv if a.startswith("--log_file=")][0]
        open(log, "a").write("[i] Initializing shader storage\\n[error] fresh problem\\n")
    sys.exit(1)
''')


class StaleLogTests(unittest.TestCase):
    def run_game(self, mode, old_log):
        tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        script = Path(tmp) / "fake.py"
        script.write_text(FAKE % mode)
        log = Path(tmp) / "forza.log"
        if old_log:
            log.write_text("[i] Initializing shader storage\n[error] yesterday problem\n")
            old = time.time() - 86400
            os.utime(log, (old, old))
        plan = SimpleNamespace(argv=[sys.executable, str(script), f"--log_file={log}"], cwd=Path(tmp),
                               env={}, unset=[], log_before_start=None)
        with mock.patch("forza_launcher.game.clean_shm"):
            with self.assertRaises(shader_prebuild.PrebuildError) as cm:
                shader_prebuild.run_game(plan, poll=0.02)
        return str(cm.exception)

    def test_stale_log_is_not_quoted(self):
        msg = self.run_game("silent", True)
        self.assertNotIn("yesterday", msg)
        self.assertEqual(msg, shader_prebuild.STOPPED_NO_LOG)

    def test_fresh_log_is_quoted(self):
        msg = self.run_game("log", True)
        self.assertIn("fresh problem", msg)
        self.assertNotIn("yesterday", msg)

    def test_unknown_flag_is_reported(self):
        self.assertEqual(self.run_game("flag", True), shader_prebuild.UNKNOWN_FLAG)


class PrebuildLogLevelTests(unittest.TestCase):
    def test_prebuild_always_logs_at_info(self):
        # #295: with Log level "warning" the done line was never written, so a finished
        # prebuild was reported as "The game stopped before the prebuild finished".
        from types import SimpleNamespace
        plan = SimpleNamespace(argv=["forza", "--log_level=warning", "--user_data_root=/x"])
        cmd = shader_prebuild.prebuild_command(plan)
        self.assertNotIn("--log_level=warning", cmd)
        self.assertEqual(cmd[-2:], ["--log_level=info", "--shader_prebuild_only=true"])
        self.assertIn("--user_data_root=/x", cmd)

if __name__ == "__main__":
    unittest.main()
