"""The "Cockpit gauges always lit" setting (#183) reaches the game's forza.toml."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import config, schema


class CockpitGaugesTest(unittest.TestCase):
    def config(self):
        return config.Config(Path(tempfile.mkdtemp()) / "launcher.toml")

    def test_off_by_default_and_not_written(self):
        setting = schema.BY_KEY["cockpit_gauges_lit"]
        self.assertEqual((setting.kind, setting.default), ("bool", False))
        self.assertEqual((setting.group, setting.section), ("Display", "Picture"))
        self.assertEqual(setting.label, "Cockpit gauges always lit")
        cfg = self.config()
        self.assertFalse(cfg.settings["cockpit_gauges_lit"])
        self.assertNotIn("cockpit_gauges_lit", config.render_toml(cfg))

    def test_on_is_written_for_the_game(self):
        cfg = self.config()
        cfg.settings["cockpit_gauges_lit"] = True
        self.assertIn("cockpit_gauges_lit = true", config.render_toml(cfg).splitlines())

    def test_shown_on_every_platform(self):
        self.assertTrue(schema.BY_KEY["cockpit_gauges_lit"].applies())


if __name__ == "__main__":
    unittest.main()
