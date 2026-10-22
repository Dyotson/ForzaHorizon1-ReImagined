"""A normal close is not a crash (#98): the "game stopped" box is for crash-like exits only, and its
"last errors" skip lines the game writes in normal play."""
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import game

HARMLESS = [
    "[2026-10-16 22:06:35.724] [error] [apu] [t17688] ffmpeg: broken frame: channel len > samples_per_frame",
    "[2026-10-16 20:11:10.760] [error] [krnl] [t3688] Unimplemented XLIVEBASE message app=000000FC, msg=0005800C",
    "[2026-10-16 22:39:42.016] [error] [krnl] [t3688] Unimplemented XAM message app=000000FE, msg=0002B004",
    "[2026-10-16 22:24:33.671] [error] [fs] [t18120] ResolvePath(AMB_Foothills.fsb) failed - device not found",
]
REAL = "[2026-10-16 22:40:00.000] [critical] [core] [t1] Unhandled read fault at host 0x1234"
INFO = "[2026-10-16 22:40:01.000] [info] [core] [t1] something"
CLOSE = ["[2026-10-15 18:06:44.058] [info] [core] [t2] Window closing, shutting down...",
         "[2026-10-15 18:06:44.060] [info] [core] [t3] Execution complete",
         "[2026-10-15 18:06:44.260] [info] [core] [t2] Title terminated; hard-exiting process."]


class ExitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / "forza.log"
        self.plan = game.LaunchPlan(game=None, cwd=Path(self.temp.name), argv=["forza", f"--log_file={self.log}"],
                                    env={}, unset=[], toml_path=Path(self.temp.name) / "t.toml", toml_text="",
                                    server_argv=None)

    def write(self, *lines):
        self.log.write_text("\n".join(lines) + "\n")

    def test_harmless_errors_are_not_listed(self):
        self.write(*HARMLESS, INFO)
        self.assertEqual(game.last_errors(self.log.read_text().splitlines()), [])
        self.assertNotIn("Last errors", game.crash_reason(self.plan, 3))

    def test_real_errors_stay(self):
        self.write(*HARMLESS, REAL, INFO)
        text = game.crash_reason(self.plan, 134)
        self.assertIn("Last errors in the log", text)
        self.assertIn("Unhandled read fault", text)
        self.assertNotIn("ffmpeg", text)
        self.assertNotIn("XLIVEBASE", text)

    def test_exit_zero_and_none_are_not_crashes(self):
        self.assertFalse(game.exit_is_crash(self.plan, 0))
        self.assertFalse(game.exit_is_crash(self.plan, None))

    def test_nonzero_exit_is_a_crash(self):
        self.write(*HARMLESS, REAL)
        self.assertTrue(game.exit_is_crash(self.plan, 1))
        self.assertTrue(game.exit_is_crash(self.plan, 134))
        self.assertTrue(game.exit_is_crash(self.plan, 3221225477))

    def test_stopped_by_the_player_is_not_a_crash(self):
        self.write(*HARMLESS, REAL)
        self.assertFalse(game.exit_is_crash(self.plan, 1, stopped=True))

    def test_log_that_ends_with_the_shutdown_lines_is_a_normal_close(self):
        self.write(*HARMLESS, *CLOSE)
        self.assertTrue(game.closed_cleanly(self.plan))
        self.assertFalse(game.exit_is_crash(self.plan, 1))

    def test_old_close_lines_do_not_hide_a_later_crash(self):
        self.write(*CLOSE, *[INFO] * 6, REAL)
        self.assertFalse(game.closed_cleanly(self.plan))
        self.assertTrue(game.exit_is_crash(self.plan, 134))

    def test_no_log_and_no_plan(self):
        self.assertTrue(game.exit_is_crash(self.plan, 1))
        self.assertTrue(game.exit_is_crash(None, 1))

    def test_signals_from_outside_on_linux(self):
        with patch.object(game.paths, "IS_WINDOWS", False):
            self.assertFalse(game.exit_is_crash(self.plan, -15))
            self.assertFalse(game.exit_is_crash(self.plan, -2))
            self.assertTrue(game.exit_is_crash(self.plan, -11))
        with patch.object(game.paths, "IS_WINDOWS", True):
            self.assertTrue(game.exit_is_crash(self.plan, -15))

    def test_relaunch_code_is_unchanged(self):
        self.assertEqual(game.RELAUNCH_EXIT_CODE, 75)


SLEEPER = "import time; time.sleep(60)"


class StopTests(unittest.TestCase):
    def session(self):
        plan = game.LaunchPlan(game=None, cwd=Path("."), argv=[sys.executable, "-c", SLEEPER], env={},
                               unset=[], toml_path=Path("t.toml"), toml_text="", server_argv=None)
        s = game.Session(plan, log=lambda t: None)
        s._start_game({})
        self.addCleanup(lambda: s.proc.kill() if s.proc.poll() is None else None)
        self.addCleanup(s.proc.wait)
        return s

    def test_stop_marks_the_exit_as_requested(self):
        s = self.session()
        s.stop()
        s.proc.wait(timeout=20)
        self.assertTrue(s.stop_requested)
        self.assertFalse(game.exit_is_crash(s.plan, s.poll(), s.stop_requested))

    def test_windows_stop_asks_the_window_then_ends_it_after_the_grace_time(self):
        s = self.session()
        with patch.object(game.paths, "IS_WINDOWS", True), \
                patch.object(game, "request_window_close", return_value=1) as ask:
            s.stop()
            ask.assert_called_once_with(s.proc.pid)
            self.assertIsNone(s.poll())          # asked, still running
            s.stop_deadline = time.monotonic() - 1
            s.poll()                             # grace over: ended
        s.proc.wait(timeout=20)
        self.assertIsNone(s.stop_deadline)

    def test_windows_stop_without_a_window_ends_it_at_once(self):
        s = self.session()
        with patch.object(game.paths, "IS_WINDOWS", True), \
                patch.object(game, "request_window_close", return_value=0):
            s.stop()
        s.proc.wait(timeout=20)


class WindowTests(unittest.TestCase):
    def poll(self, code, stopped, crash):
        from types import SimpleNamespace
        from unittest.mock import MagicMock
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        try:
            from forza_launcher.ui.window import MainWindow
        except ImportError:
            self.skipTest("PySide6 is not installed")
        session = MagicMock()
        session.poll.return_value = code
        session.stop_requested = stopped
        win = SimpleNamespace(session=session, session_plan=None, poll=MagicMock(), play=MagicMock(),
                              _prepare_crash_report=MagicMock(), _close_after_game=False)
        with patch.object(game, "crash_reason", return_value="CRASH BOX"):
            MainWindow._poll(win)
        self.assertEqual(win._prepare_crash_report.called, crash)
        return win.play.show_message.call_args_list

    def test_user_stop_shows_closed(self):
        calls = self.poll(1, True, False)
        self.assertEqual(calls[0].args[0], "The game closed.")

    def test_clean_exit_shows_closed(self):
        self.assertEqual(self.poll(0, False, False)[0].args[0], "The game closed.")

    def test_crash_shows_the_box(self):
        calls = self.poll(134, False, True)
        self.assertEqual(calls[0].args[0], "CRASH BOX")
        self.assertTrue(calls[0].args[1])


if __name__ == "__main__":
    unittest.main()
