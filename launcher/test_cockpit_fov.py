"""Cockpit camera config reaches the runtime without changing stock defaults."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import schema
from forza_launcher.config import Config, render_toml


class CockpitFovTests(unittest.TestCase):
    def test_schema_range_and_units(self):
        setting = schema.BY_KEY["forza_cockpit_fov"]
        self.assertEqual(setting.default, 0)
        self.assertIn("vertical degrees", setting.label)
        self.assertEqual([value for value, _ in setting.choices], [0] + list(range(40, 91)))
        for value in (0, 40, 62, 90):
            self.assertEqual(schema.coerce(setting, str(value)), value)
        for value in (-1, 1, 39, 91, 62.5, "nan", "inf"):
            self.assertEqual(schema.coerce(setting, value), 0)

    def test_toml_restart_and_reset(self):
        with tempfile.TemporaryDirectory() as directory:
            cfg = Config(Path(directory) / "launcher.json")
            cfg.advanced["data_root"] = directory
            cfg.settings["frame_rate"] = 30  # Avoid Match display's hardware query.
            self.assertNotIn("forza_cockpit_fov =", render_toml(cfg))
            cfg.settings["forza_cockpit_fov"] = 75
            cfg.save()
            restarted = Config(cfg.path)
            self.assertIn("forza_cockpit_fov = 75\n", render_toml(restarted))
            restarted.settings["forza_cockpit_fov"] = 0
            restarted.save()
            self.assertNotIn("forza_cockpit_fov =", render_toml(Config(cfg.path)))


if __name__ == "__main__":
    unittest.main()
