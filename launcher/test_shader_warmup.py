import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).parent))
from forza_launcher.shader_warmup import Monitor, Status, _LIMIT


class WarmupLogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "game.log"

    def append(self, text):
        with self.path.open("ab") as out:
            out.write(text.encode())

    def test_missing_old_and_rotated_logs(self):
        monitor = Monitor(self.path)
        self.assertIsNone(monitor.poll())
        self.append("SHADER_WARMUP: Vulkan begin\n")
        self.assertTrue(monitor.poll().active)
        self.append("SHADER_WARMUP: Vulkan complete in 4 ms\n")
        self.assertFalse(monitor.poll().active)
        stale = Monitor(self.path)
        self.assertIsNone(stale.poll(), "A previous game's log must not appear as current progress")
        replacement = self.path.with_suffix(".new")
        replacement.write_text("SHADER_WARMUP: D3D12 begin\n")
        replacement.replace(self.path)
        self.assertEqual(stale.poll(), Status("D3D12", True))

    def test_fragmented_lines_and_worker_order(self):
        monitor = Monitor(self.path)
        self.append("[info] SHADER_WARMUP: Vulkan beg")
        self.assertIsNone(monitor.poll())
        self.append("in\r\nSHADER_WARMUP: Vulkan progress 32/96\n")
        self.assertEqual(monitor.poll(), Status("Vulkan", True, 32, 96))
        self.append("SHADER_WARMUP: Vulkan progress 16/96\n")
        self.assertEqual(monitor.poll().done, 32)
        self.append("SHADER_WARMUP: Vulkan progress 96/96\nSHADER_WARMUP: Vulkan complete in 234 ms\n")
        self.assertFalse(monitor.poll().active)
        self.append("SHADER_WARMUP: Vulkan begin\n")
        self.assertEqual(monitor.poll(), Status("Vulkan", True))

    def test_invalid_counters_and_untrusted_text(self):
        monitor = Monitor(self.path)
        self.append("SHADER_WARMUP: Vulkan begin\n")
        before = monitor.poll()
        for line in ("progress 5/0", "progress 8/2", "progress 1/9999999999",
                     "progress -1/4", "progress " + "1" * 5000 + "/4", "complete in -1 ms",
                     "progress <b>1</b>/4"):
            self.append(f"SHADER_WARMUP: Vulkan {line}\n")
            self.assertEqual(monitor.poll(), before)

    def test_large_logs_do_not_require_reading_the_whole_file(self):
        monitor = Monitor(self.path)
        self.append("x" * (_LIMIT * 3) + "\nSHADER_WARMUP: Vulkan progress 16/32\n")
        self.assertEqual(monitor.poll(), Status("Vulkan", True, 16, 32))
        self.append("SHADER_WARMUP: Vulkan complete in 300 ms\n")
        self.assertFalse(monitor.poll().active)

    def test_no_extra_line_is_dropped_at_exact_read_boundaries(self):
        monitor = Monitor(self.path)
        self.append("x" * (_LIMIT - 1) + "\n")
        self.assertIsNone(monitor.poll())
        message = "SHADER_WARMUP: Vulkan progress 1/2\n"
        self.append(message + "x" * (_LIMIT - len(message) - 1) + "\n")
        self.assertEqual(monitor.poll(), Status("Vulkan", True, 1, 2))


try:
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None


@unittest.skipIf(QApplication is None, "PySide6 is not installed")
class WarmupUITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from forza_launcher.config import Config
        from forza_launcher.ui.window import MainWindow
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        cfg = Config(Path(self.temp.name) / "launcher.json")
        cfg.advanced["data_root"] = str(Path(self.temp.name) / "data")
        cfg.gpu["checked"] = True
        cfg.loaded = True
        cfg.d["check_updates"] = False
        self.win = MainWindow(cfg)
        self.addCleanup(self.win.close)

    def test_progress_does_not_overwrite_warnings_and_resets(self):
        page = self.win.play
        page.show_message("A DLC package is missing", warn=True, keep=True)
        page.set_shader_warmup(Status("Vulkan", True))
        self.assertIsNotNone(page.install_details.parentWidget())
        self.assertIsNotNone(page.install_paths.parentWidget())
        self.assertFalse(page.shader_progress.isHidden())
        self.assertEqual(page.shader_progress.maximum(), 0)
        page.set_shader_warmup(Status("Vulkan", True, 16, 32))
        self.assertEqual(page.shader_progress.value(), 16)
        self.assertIn("16 of 32", page.shader_label.text())
        self.assertEqual(page.warn.text(), "A DLC package is missing")
        page.set_shader_warmup(Status("Vulkan", False))
        self.assertTrue(page.shader_progress.isHidden())
        page.set_shader_warmup(None)
        self.assertTrue(page.shader_label.isHidden())

    def test_driver_cache_control_requires_vulkan_and_shader_storage(self):
        page = self.win.settings
        row = page.rows["vulkan_store_pipeline_cache"]
        for backend, stored, enabled in (("vulkan", True, True), ("d3d12", True, False),
                                         ("vulkan", False, False), ("vulkan", True, True)):
            self.win.cfg.settings.update(gpu_backend=backend, store_shaders=stored)
            page._apply_dependencies()
            self.assertEqual(row.control.isEnabled(), enabled)
            if enabled:
                self.assertEqual(row.dependency.text(), "")
            else:
                self.assertIn("Vulkan", row.dependency.text())


if __name__ == "__main__":
    unittest.main()
