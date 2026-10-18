"""HDR settings persist, generate game configuration, and follow backend availability."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import schema
from forza_launcher.config import Config, import_toml, render_toml

try:
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None


class HdrSettings(unittest.TestCase):
    def config(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        return Config(Path(folder.name) / "launcher.json")

    def test_default_does_not_change_sdr_configuration(self):
        cfg = self.config()
        self.assertFalse(cfg.settings["present_hdr"])
        self.assertNotIn("present_hdr", render_toml(cfg))

    def test_import_save_and_game_configuration_roundtrip(self):
        cfg = self.config()
        expected = dict(present_hdr=True, present_hdr_paper_white_nits=160.0,
                        present_hdr_peak_nits=400.0, present_hdr_highlights=0.75,
                        present_hdr_gamma=0.0, present_hdr_compare=True,
                        present_hdr_test_pattern=3)
        cfg.settings.update(expected)
        cfg.save()
        loaded = Config(cfg.path)
        imported = self.config()
        import_toml(imported, render_toml(loaded))
        for key, value in expected.items():
            with self.subTest(key=key):
                self.assertEqual(loaded.settings[key], value)
                self.assertEqual(imported.settings[key], value)
        self.assertNotIn("present_hdr", imported.advanced.get("extras_toml", ""))

    def test_invalid_brightness_cannot_reach_game_configuration(self):
        for key in ("present_hdr_paper_white_nits", "present_hdr_peak_nits",
                    "present_hdr_highlights", "present_hdr_gamma"):
            setting = schema.BY_KEY[key]
            for value in (float("nan"), float("inf"), -float("inf"), "invalid"):
                with self.subTest(key=key, value=value):
                    self.assertEqual(schema.coerce(setting, value), setting.default)
            self.assertEqual(schema.coerce(setting, -100), setting.lo)
            self.assertEqual(schema.coerce(setting, 100000), setting.hi)
        self.assertEqual(schema.coerce(schema.BY_KEY["present_hdr_test_pattern"], 99), 0)

    @unittest.skipIf(QApplication is None, "PySide6 is not installed")
    def test_controls_follow_hdr_and_backend_without_losing_values(self):
        from forza_launcher.ui.settings_page import SettingsPage
        app = QApplication.instance() or QApplication([])
        cfg = self.config()
        cfg.settings.update(present_hdr_peak_nits=400.0)
        window = MagicMock()
        window.preview_text.return_value = ""
        page = SettingsPage(cfg, window)
        self.addCleanup(page.close)
        self.assertFalse(page.rows["present_hdr_peak_nits"].control.isEnabled())
        page.set_value("present_hdr", True)
        self.assertTrue(page.rows["present_hdr_peak_nits"].control.isEnabled())
        # The Direct3D setting exists only on Windows.
        cfg.settings["gpu_backend"] = "d3d12"
        page._apply_dependencies()
        self.assertFalse(page.rows["present_hdr"].control.isEnabled())
        self.assertFalse(page.rows["present_hdr_peak_nits"].control.isEnabled())
        self.assertTrue(cfg.settings["present_hdr"])
        self.assertEqual(cfg.settings["present_hdr_peak_nits"], 400.0)
        cfg.settings["gpu_backend"] = "vulkan"
        page._apply_dependencies()
        self.assertTrue(page.rows["present_hdr_peak_nits"].control.isEnabled())
        page.flush()
        app.processEvents()


if __name__ == "__main__":
    unittest.main()
