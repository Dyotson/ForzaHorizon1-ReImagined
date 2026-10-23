"""The experimental native pause menu uses the ordinary launcher setting/TOML path."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import config, schema  # noqa: E402


class NativeMenuSettings(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "launcher.json"
        self.cfg = config.Config(self.path)
        self.cfg.advanced["data_root"] = self.temp.name

    def test_opt_in_setting_is_available_in_advanced_menu_on_every_platform(self):
        setting = schema.BY_KEY["forza_native_pause"]
        self.assertEqual((setting.group, setting.section, setting.kind), ("Advanced", "Menu", "bool"))
        self.assertEqual(setting.label, "Native pause menu (experimental)")
        self.assertTrue(setting.restart)
        self.assertEqual(setting.platform, "")
        self.assertIn(setting, schema.for_group("Advanced"))
        self.assertIs(self.cfg.settings[setting.key], False)
        self.assertNotIn(setting.key, config.parse_toml(config.render_toml(self.cfg)))

    def test_enable_survives_save_and_reaches_game_configuration(self):
        self.cfg.settings["forza_native_pause"] = True
        self.cfg.save()
        restored = config.Config(self.path)
        self.assertIs(restored.settings["forza_native_pause"], True)
        rendered = config.parse_toml(config.render_toml(restored))
        self.assertIs(rendered["forza_native_pause"], True)
        self.assertFalse(any(key.startswith("fh2_") for key in rendered))

    def test_tuning_is_independently_opt_in_and_reaches_game_configuration(self):
        setting = schema.BY_KEY["forza_car_tuning"]
        self.assertIs(self.cfg.settings[setting.key], False)
        self.assertNotIn(setting.key, config.parse_toml(config.render_toml(self.cfg)))
        self.assertTrue(setting.restart)
        self.cfg.settings[setting.key] = True
        self.cfg.save()
        restored = config.Config(self.path)
        self.assertIs(config.parse_toml(config.render_toml(restored))[setting.key], True)

    def test_import_is_managed_setting_and_disabling_restores_default(self):
        config.import_toml(self.cfg, "forza_native_pause = true\n")
        self.assertIs(self.cfg.settings["forza_native_pause"], True)
        self.assertNotIn("forza_native_pause", self.cfg.advanced["extras_toml"])
        config.import_toml(self.cfg, "forza_native_pause = false\n")
        self.cfg.save()
        restored = config.Config(self.path)
        self.assertIs(restored.settings["forza_native_pause"], False)
        self.assertNotIn("forza_native_pause", config.parse_toml(config.render_toml(restored)))


if __name__ == "__main__":
    unittest.main()
