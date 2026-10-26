"""The experimental car tuning (docs/car-tuning.md) uses the ordinary launcher setting/TOML path, and
Play picks runtime/update-native only while it is on."""
from pathlib import Path
from unittest import mock
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import config, crash_report, schema, game  # noqa: E402


class NativeMenuSettings(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "launcher.json"
        self.cfg = config.Config(self.path)
        self.cfg.advanced["data_root"] = self.temp.name
        self.cfg.online["mode"] = "offline"

    def test_tuning_is_opt_in_in_the_gameplay_tab_on_every_platform(self):
        setting = schema.BY_KEY["forza_car_tuning"]
        self.assertEqual([s.key for s in schema.S].count(setting.key), 1)  # one switch, in one tab
        self.assertEqual((setting.group, setting.section, setting.kind), ("Gameplay", "Pause menu", "bool"))
        self.assertEqual(setting.label, "Car tuning (experimental)")
        self.assertTrue(setting.restart)
        self.assertEqual(setting.platform, "")
        self.assertIn(setting, schema.for_group("Gameplay"))
        self.assertIs(self.cfg.settings[setting.key], False)
        self.assertNotIn(setting.key, config.parse_toml(config.render_toml(self.cfg)))
        self.assertIn("forza_car_tuning", crash_report.SETTING_KEYS)

    def test_the_game_defines_the_cvar_with_the_same_default(self):
        hook = Path(__file__).resolve().parents[1] / "forza-recomp/src/hooks/native_car_tuning.cpp"
        self.assertIn("REXCVAR_DEFINE_BOOL(forza_car_tuning, false,", hook.read_text())

    def test_enable_survives_save_and_reaches_game_configuration(self):
        setting = schema.BY_KEY["forza_car_tuning"]
        self.cfg.settings[setting.key] = True
        self.cfg.save()
        restored = config.Config(self.path)
        self.assertIs(restored.settings[setting.key], True)
        self.assertIs(config.parse_toml(config.render_toml(restored))[setting.key], True)

    def install(self, *names):
        folder = Path(self.temp.name) / "runtime"
        for name in names:
            (folder / name).mkdir(parents=True)
        return game.GameInstall(folder, folder / game.EXE)

    def test_restart_toggles_select_stock_or_prepared_overlay_without_rebuild(self):
        install = self.install("update", "update-native")
        with mock.patch.dict(game.os.environ, {}, clear=True):
            for tuning in (False, True, False):
                with self.subTest(tuning=tuning):
                    self.cfg.settings.update(forza_car_tuning=tuning)
                    chosen = game.update_data_root(self.cfg, install, config.render_toml(self.cfg))
                    self.assertEqual(chosen.name, "update-native" if tuning else "update")
            (install.dir / "update-native").rmdir()
            self.cfg.settings["forza_car_tuning"] = True
            self.assertEqual(game.update_data_root(self.cfg, install, config.render_toml(self.cfg)).name, "update")

    def plan(self, install):
        with mock.patch.dict(game.os.environ, {}, clear=True):
            return game.build_plan(self.cfg, install, write=False, log=lambda _t: None)

    def test_play_mounts_the_prepared_update_only_with_tuning_on(self):
        install = self.install("update", "update-native")
        self.assertIn(f"--update_data_root={install.dir / 'update'}", self.plan(install).argv)
        self.cfg.settings["forza_car_tuning"] = True
        plan = self.plan(install)
        self.assertIn(f"--update_data_root={install.dir / 'update-native'}", plan.argv)
        self.assertFalse(any("Car tuning" in note for note in plan.notes))
        self.cfg.advanced["extra_args"] = "--update_data_root=/elsewhere"
        self.assertFalse(any(a.startswith("--update_data_root=" + str(install.dir))
                             for a in self.plan(install).argv))

    def test_play_says_when_the_build_has_no_tuning_resources(self):
        install = self.install("update")
        self.assertFalse(any("Car tuning" in note for note in self.plan(install).notes))
        self.cfg.settings["forza_car_tuning"] = True
        plan = self.plan(install)
        self.assertIn(f"--update_data_root={install.dir / 'update'}", plan.argv)
        self.assertTrue(any("Car tuning is on" in note and "Rebuild" in note for note in plan.notes))

    def test_cli_and_environment_disable_override_enabled_toml(self):
        text = "forza_car_tuning = true\n"
        with mock.patch.dict(game.os.environ, {}, clear=True):
            self.assertTrue(game.native_update_requested(self.cfg, text))
            self.cfg.advanced["extra_env"] = "REX_FORZA_CAR_TUNING=false"
            self.assertFalse(game.native_update_requested(self.cfg, text))
            self.cfg.advanced["extra_args"] = "--forza_car_tuning=true"
            self.assertTrue(game.native_update_requested(self.cfg, text))
            self.cfg.advanced["extra_args"] = "--forza_car_tuning=false"
            self.assertFalse(game.native_update_requested(self.cfg, text))
            self.cfg.advanced["extra_args"] = "--forza_car_tuning false"
            self.assertTrue(game.native_update_requested(self.cfg, text))  # Bare flag, positional value.
            self.cfg.advanced["extra_args"] = "--no-forza_car_tuning"
            self.assertFalse(game.native_update_requested(self.cfg, text))
            self.cfg.advanced["extra_args"] = "--no-forza_car_tuning=true"
            self.assertFalse(game.native_update_requested(self.cfg, text))
            self.cfg.advanced["extra_args"] = "--no-forza_car_tuning=false"
            self.assertTrue(game.native_update_requested(self.cfg, text))
        with mock.patch.dict(game.os.environ, {"REX_FORZA_CAR_TUNING": "1"}, clear=True):
            self.cfg.advanced["extra_env"] = self.cfg.advanced["extra_args"] = ""
            self.assertTrue(game.native_update_requested(self.cfg, ""))

    def test_import_is_managed_setting_and_disabling_restores_default(self):
        config.import_toml(self.cfg, "forza_car_tuning = true\n")
        self.assertIs(self.cfg.settings["forza_car_tuning"], True)
        self.assertNotIn("forza_car_tuning", self.cfg.advanced["extras_toml"])
        config.import_toml(self.cfg, "forza_car_tuning = false\n")
        self.cfg.save()
        restored = config.Config(self.path)
        self.assertIs(restored.settings["forza_car_tuning"], False)
        self.assertNotIn("forza_car_tuning", config.parse_toml(config.render_toml(restored)))


if __name__ == "__main__":
    unittest.main()
