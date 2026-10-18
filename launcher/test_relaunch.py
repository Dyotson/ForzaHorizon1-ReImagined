"""The game restarting itself (exit code 75, src/kernel/xam_loader.cpp): the launcher starts it
again with the same working directory and environment, keeps the local server, and gives up on a
game that keeps asking."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import game

# A stand-in for the game: appends its environment to a file, exits with the code listed in
# CODES (comma separated, one per start; the last one repeats).
FAKE_GAME = r"""
import os, sys
rec = sys.argv[1]
n = len(open(rec).read().splitlines()) if os.path.exists(rec) else 0
with open(rec, "a") as f:
    f.write("%s|%s|%s|%s\n" % (os.getcwd(), os.environ.get("FORZA_RELAUNCHED", ""),
            os.environ.get("FORZA_LAUNCHER_RELAUNCH", ""), os.environ.get("PLAN_VAR", "")))
codes = os.environ["CODES"].split(",")
sys.exit(int(codes[min(n, len(codes) - 1)]))
"""


class RelaunchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        # Cleanups run last-in-first-out: sessions must release processes and file
        # handles before the temporary directory is removed, especially on Windows.
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        (self.dir / "cwd").mkdir()
        self.record = self.dir / "starts.txt"
        (self.dir / "fake.py").write_text(FAKE_GAME)
        self.messages = []

    def session(self, codes, server=False):
        env = mock.patch.dict(os.environ, CODES=codes)
        env.start()
        self.addCleanup(env.stop)
        server_argv = None
        if server:
            (self.dir / "srv").mkdir()
            server_argv = [sys.executable, "-c", "import time; print('server started', flush=True); time.sleep(60)",
                           "--data-dir", str(self.dir / "srv")]
        plan = game.LaunchPlan(game=None, cwd=self.dir / "cwd",
                               argv=[sys.executable, str(self.dir / "fake.py"), str(self.record)],
                               env={"PLAN_VAR": "x"}, unset=["FORZA_RELAUNCHED"], toml_path=self.dir / "t.toml",
                               toml_text="", server_argv=server_argv)
        s = game.Session(plan, log=self.messages.append)
        self.addCleanup(self.cleanup_session, s)
        return s

    def cleanup_session(self, s):
        s.cleanup()
        # Also reap children if an assertion fails before the normal wait path.
        for child in (s.proc, s.server):
            if child is not None:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=10)

    def wait(self, s):
        s.proc.wait(timeout=30)
        return s.poll()

    def starts(self):
        return [line.split("|") for line in self.record.read_text().splitlines()]

    def test_server_log_parent_handle_is_closed_after_spawn(self):
        s = self.session("0", server=True)
        with mock.patch.object(game.subprocess, "Popen") as spawn:
            s.start()
        self.assertTrue(spawn.call_args_list[0].kwargs["stdout"].closed)

    def test_server_log_is_closed_when_spawn_fails(self):
        s = self.session("0", server=True)
        with mock.patch.object(game.subprocess, "Popen", side_effect=OSError("cannot start server")) as spawn:
            with self.assertRaisesRegex(OSError, "cannot start server"):
                s.start()
        self.assertTrue(spawn.call_args.kwargs["stdout"].closed)
        self.assertIsNone(s.server)

    def test_exit_75_then_0_restarts_once_with_same_cwd_and_env(self):
        s = self.session("75,0", server=True)
        s.start()
        server = s.server
        self.assertEqual(self.wait(s), game.RELAUNCH_EXIT_CODE)
        self.assertTrue(s.relaunch())
        self.assertIs(s.server, server)
        self.assertIsNone(server.poll(), "the local server must keep running")
        self.assertEqual(self.wait(s), 0)
        first, second = self.starts()
        cwd = str(self.dir / "cwd")
        self.assertEqual(first, [cwd, "", "1", "x"])      # the first start is not a restart
        self.assertEqual(second, [cwd, "1", "1", "x"])    # the restart is told so, same cwd and env
        self.assertIn("The game restarts itself", self.messages)
        s.cleanup()
        server.wait(timeout=10)
        self.assertIn("server started", (self.dir / "srv" / "server.log").read_text())

    def test_a_restart_loop_is_bounded(self):
        s = self.session("75")   # every start asks for another
        s.start()
        restarts = 0
        for _ in range(game.RELAUNCH_LIMIT + 3):
            self.assertEqual(self.wait(s), game.RELAUNCH_EXIT_CODE)
            if not s.relaunch():
                break
            restarts += 1
        self.assertEqual(restarts, game.RELAUNCH_LIMIT)
        self.assertEqual(len(self.starts()), game.RELAUNCH_LIMIT + 1)
        self.assertIn("The game keeps restarting itself; not starting it again", self.messages)

    def test_restarts_spaced_over_the_window_are_not_limited(self):
        s = self.session("0")
        s.start()
        self.wait(s)
        s.relaunches = [-10 * game.RELAUNCH_WINDOW] * game.RELAUNCH_LIMIT   # long ago
        self.assertTrue(s.relaunch())
        self.wait(s)


class WindowPollTests(unittest.TestCase):
    """MainWindow._poll on code 75: a restart keeps the session; a refused one ends it."""

    def run_poll(self, relaunch_result):
        from types import SimpleNamespace
        from unittest.mock import MagicMock
        try:
            os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
            from forza_launcher.ui.window import MainWindow
        except ImportError:
            self.skipTest("PySide6 is not installed")
        session = MagicMock()
        session.poll.return_value = game.RELAUNCH_EXIT_CODE
        session.relaunch.return_value = relaunch_result
        win = SimpleNamespace(session=session, session_plan=None, poll=MagicMock(), play=MagicMock())
        MainWindow._poll(win)
        return win, session

    def test_restart_keeps_the_session(self):
        win, session = self.run_poll(True)
        self.assertIs(win.session, session)
        session.cleanup.assert_not_called()
        win.poll.stop.assert_not_called()

    def test_refused_restart_ends_the_session(self):
        from unittest.mock import patch
        with patch.object(game, "crash_reason", return_value="stopped"):
            win, session = self.run_poll(False)
        self.assertIsNone(win.session)
        session.cleanup.assert_called_once()
        win.poll.stop.assert_called_once()


if __name__ == "__main__":
    unittest.main()
