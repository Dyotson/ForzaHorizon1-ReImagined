"""The "Match display" frame rate (#215): monitor index to rate, clamping, fallback, config and in-game sync."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from forza_launcher import display, ingame, schema, specs
from forza_launcher.config import Config, render_toml

LOW_CPU = specs.Specs(cpu_name="Old", cpu_cores=2, cpu_threads=4, avx2=False, ram_gb=8, gpu_name="X", vram_gb=8)
MID_CPU = specs.Specs(cpu_name="Mid", cpu_cores=4, cpu_threads=8, avx2=True, ram_gb=16, gpu_name="X", vram_gb=8)
FAST = specs.Specs(cpu_name="Fast", cpu_cores=8, cpu_threads=16, avx2=True, ram_gb=32, gpu_name="RTX 4080", vram_gb=16)


class ResolveTests(unittest.TestCase):
    rates = [180.0, 60.0, 144.0]

    def test_monitor_index_picks_the_screen(self):
        for monitor, hz in [(0, 180), (1, 180), (2, 60), (3, 144), (9, 180)]:
            with self.subTest(monitor=monitor):
                self.assertEqual(display.resolve(monitor, rates=self.rates), hz)

    def test_clamping_and_rounding(self):
        for hz, want in [(59.94, 60), (143.98, 144), (30.0, 30), (24.0, 30), (360.0, 240), (165.0, 165)]:
            self.assertEqual(display.resolve(0, rates=[hz]), want)

    def test_fallback_is_60_and_logged(self):
        for rates in ([], [0.0], [-1.0]):
            lines = []
            self.assertEqual(display.resolve(0, rates=rates, log=lines.append), 60)
            self.assertTrue(lines and "60" in lines[0])

    def test_never_above_what_the_specs_accept(self):
        self.assertEqual(display.resolve(0, LOW_CPU, rates=[180.0]), 30)
        self.assertEqual(display.resolve(0, MID_CPU, rates=[180.0]), 60)
        self.assertEqual(display.resolve(0, FAST, rates=[180.0]), 180)
        for pc in (LOW_CPU, MID_CPU, FAST):
            rate = display.resolve(0, pc, rates=[240.0])
            self.assertFalse([w for w in specs.warnings(pc, {"frame_rate": rate}) if w.key == "frame_rate"])

    def test_label(self):
        self.assertEqual(display.label(2, self.rates), "Match display (60 Hz)")
        self.assertEqual(display.label(0, []), "Match display")

    def test_qt_screens(self):
        from PySide6.QtWidgets import QApplication
        app = QApplication.instance() or QApplication([])
        self.assertIsNotNone(app)
        rates = display.screen_rates()
        self.assertTrue(rates and all(r > 0 for r in rates))


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cfg = Config(self.root / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.root)

    def test_zero_survives_coerce_and_a_save(self):
        setting = schema.BY_KEY["frame_rate"]
        self.assertEqual(schema.coerce(setting, 0), 0)
        self.assertEqual(schema.coerce(setting, 45), 40)
        self.cfg.settings["frame_rate"] = 0
        self.cfg.save()
        self.assertEqual(Config(self.cfg.path).settings["frame_rate"], 0)

    def test_toml_gets_a_number(self):
        self.cfg.settings["frame_rate"] = 0
        from unittest.mock import patch
        with patch.object(display, "screen_rates", return_value=[180.0]):
            self.assertIn("frame_rate = 180\n", render_toml(self.cfg))
        self.assertEqual(self.cfg.settings["frame_rate"], 0)

    def test_warnings_see_the_resolved_rate(self):
        from unittest.mock import patch
        self.cfg.settings["frame_rate"] = 0
        self.cfg.d["specs"]["values"] = {"cpu_name": "Mid", "cpu_cores": 4, "avx2": True, "ram_gb": 16,
                                         "gpu_name": "RTX 4080", "vram_gb": 16}
        with patch.object(display, "screen_rates", return_value=[180.0]):
            self.assertEqual(self.cfg.effective_settings()["frame_rate"], 60)
            self.assertFalse(self.cfg.specs_warnings())

    def request(self, **values):
        (self.root / ingame.FILENAME).write_text(json.dumps({"version": 1, "settings": values}))

    def test_ingame_explicit_choice_overrides(self):
        from unittest.mock import patch
        self.cfg.settings["frame_rate"] = 0
        with patch.object(display, "screen_rates", return_value=[180.0]):
            self.request(frame_rate="120")
            self.assertTrue(ingame.import_pending(self.cfg, lambda _: None))
        self.assertEqual(Config(self.cfg.path).settings["frame_rate"], 120)

    def test_ingame_apply_without_change_keeps_match_display(self):
        from unittest.mock import patch
        self.cfg.settings["frame_rate"] = 0
        with patch.object(display, "screen_rates", return_value=[180.0]):
            self.request(frame_rate="180", vsync="false")
            self.assertTrue(ingame.import_pending(self.cfg, lambda _: None))
        saved = Config(self.cfg.path).settings
        self.assertEqual(saved["frame_rate"], 0)
        self.assertFalse(saved["vsync"])

    def test_use_recommended_keeps_match_display_when_safe(self):
        from unittest.mock import patch
        self.cfg.settings["frame_rate"] = 0
        self.cfg.d["specs"]["values"] = {"cpu_name": "Old", "cpu_cores": 2, "avx2": False, "ram_gb": 8}
        with patch.object(display, "screen_rates", return_value=[144.0]):
            self.assertEqual(self.cfg.effective_settings()["frame_rate"], 30)
            self.cfg.use_recommended()
            self.assertEqual(self.cfg.settings["frame_rate"], 0)
            self.assertFalse([w for w in self.cfg.specs_warnings() if w.key == "frame_rate"])


class ControlTests(unittest.TestCase):
    def test_combo_has_match_display_first(self):
        from PySide6.QtWidgets import QApplication
        app = QApplication.instance() or QApplication([])
        from forza_launcher.ui.settings_page import make_control
        values = []
        control = make_control(schema.BY_KEY["frame_rate"], 0, values.append, monitor=0)
        self.assertTrue(control.itemText(0).startswith("Match display"))
        self.assertEqual(control.currentIndex(), 0)
        control.setCurrentIndex(3)
        control.setCurrentIndex(0)
        self.assertEqual(values[-1], 0)
        control.setCurrentIndex(control.findData(60))
        self.assertEqual(values[-1], 60)
        control.setCurrentIndex(1)
        self.assertEqual(values[-1], 20)
        self.assertIsNotNone(app)


if __name__ == "__main__":
    unittest.main()
