"""The window stays responsive (#124): tool detection, the title update check and the project
source hash are kept instead of redone at every page change; the DLC folder, the title update and
the tools are looked at off the GUI thread; the same start error is logged once."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import build_env, builder, installer

try:
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None


class CacheTests(unittest.TestCase):
    def setUp(self):
        build_env.forget_tools()
        self.addCleanup(build_env.forget_tools)

    def test_tools_are_detected_once_per_session(self):
        root = Path("/nonexistent-root")
        with patch.object(build_env, "_detect_tools", side_effect=lambda r, p=False: build_env.Tools()) as det:
            self.assertIsNone(build_env.cached_tools(root))
            first = build_env.detect_tools(root)
            self.assertIs(build_env.detect_tools(root), first)
            self.assertIs(build_env.cached_tools(root), first)
            self.assertEqual(det.call_count, 1)
            build_env.detect_tools(root, max_age=0)        # a build looks again
            self.assertEqual(det.call_count, 2)
            build_env.forget_tools()
            self.assertIsNone(build_env.cached_tools(root))
            build_env.detect_tools(root)
            self.assertEqual(det.call_count, 3)

    def test_title_update_is_inspected_once_per_file_state(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "tu"
            f.write_bytes(b"x" * 100)
            self.assertIsNone(installer.title_update_cached(str(f)))
            with patch.object(installer, "_inspect_title_update", return_value=(False, "no")) as ins:
                self.assertEqual(installer.inspect_title_update(str(f)), (False, "no"))
                installer.inspect_title_update(str(f))
                self.assertEqual(ins.call_count, 1)
                self.assertEqual(installer.title_update_cached(str(f)), (False, "no"))
                f.write_bytes(b"y" * 200)                  # another file: asked again
                installer.inspect_title_update(str(f))
                self.assertEqual(ins.call_count, 2)

    def test_sources_hash_reads_files_only_when_they_changed(self):
        with tempfile.TemporaryDirectory() as d:
            proj = Path(d)
            src = proj / "forza-recomp"
            src.mkdir()
            f = src / "a.cpp"
            f.write_bytes(b"one")
            os.utime(f, ns=(10**18, 10**18))
            h1 = builder.sources_hash(proj)
            f.write_bytes(b"two")
            os.utime(f, ns=(10**18, 10**18))               # same size and time: the memo answers
            self.assertEqual(builder.sources_hash(proj), h1)
            os.utime(f, ns=(10**18 + 10**9, 10**18 + 10**9))
            self.assertNotEqual(builder.sources_hash(proj), h1)


@unittest.skipIf(QApplication is None, "PySide6 is not installed")
class WindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from forza_launcher.config import Config
        from forza_launcher.ui.window import MainWindow
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        p = patch("forza_launcher.paths.config_dir", return_value=Path(self.temp.name))
        p.start()
        self.addCleanup(p.stop)
        build_env.forget_tools()
        self.addCleanup(build_env.forget_tools)
        cfg = Config(Path(self.temp.name) / "launcher.json")
        cfg.gpu["checked"] = True
        cfg.loaded = True
        cfg.d["check_updates"] = False
        cfg.d["advanced"]["data_root"] = self.temp.name
        self.cfg = cfg
        self.win = MainWindow(cfg)
        self.wait(self.win.setup)       # the detection the window started itself

    def tearDown(self):
        self.win.close()

    def wait(self, page):
        end = time.time() + 20
        while page._probes and time.time() < end:
            self.app.processEvents()
            time.sleep(0.005)
        self.app.processEvents()
        self.assertFalse(page._probes)

    def test_setup_does_not_start_programs_on_the_gui_thread(self):
        import threading
        build_env.forget_tools()
        self.win.setup._probe_done.clear()
        main = threading.get_ident()
        seen = []

        def fake(root, prefer=False):
            seen.append(threading.get_ident())
            return build_env.Tools()
        with patch.object(build_env, "_detect_tools", side_effect=fake):
            self.win.setup.refresh()
            self.win.setup.refresh()                       # still running: not started twice
            self.wait(self.win.setup)
            self.win.setup.refresh()                       # cached: nothing runs
            self.wait(self.win.setup)
        self.assertEqual(len(seen), 1)
        self.assertNotEqual(seen[0], main)

    def test_unreadable_dlc_folder_is_asked_once_and_shown(self):
        missing = str(Path(self.temp.name) / "nope")
        setup = self.win.setup
        setup.dlc.setText(missing)
        calls = []
        real = installer.describe_dlc_folder

        def counting(folder):
            calls.append(folder)
            return real(folder)
        with patch.object(installer, "describe_dlc_folder", side_effect=counting):
            setup._dlc_changed()
            self.wait(setup)
            setup.refresh()
            setup.refresh()
            self.wait(setup)
        self.assertEqual(len(calls), 1)
        self.assertIn("is not a folder", setup.dlclist.text())

    def test_dlc_folder_is_listed_after_the_background_read(self):
        d = Path(self.temp.name) / "dlc"
        (d / "4D5309C9").mkdir(parents=True)
        setup = self.win.setup
        setup.dlc.setText(str(d))
        setup._dlc_changed()
        self.assertEqual(setup.dlclist.text(), "Reading the DLC folder...")
        self.wait(setup)
        self.assertIn("No DLC found", setup.dlclist.text())

    def test_same_start_error_is_logged_once(self):
        from forza_launcher import game
        err = RuntimeError("No Forza Horizon save at X")
        install = type("I", (), {"dir": Path(self.temp.name)})()
        with patch.object(game, "find_game", return_value=(install, [])), \
                patch.object(installer, "check_game_folder", return_value=(True, "")), \
                patch.object(game, "build_plan", side_effect=err), \
                patch("forza_launcher.paths.log_error") as log:
            for _ in range(3):
                self.win.start_game()
            self.assertEqual(log.call_count, 1)


if __name__ == "__main__":
    unittest.main()
