from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import config, crash_report, game, power, schema
from forza_launcher.config import Config


class TexturePackTests(unittest.TestCase):
    """Settings > Display > Texture packs (#384, docs/texture_packs.md)."""

    def setUp(self):
        platform = patch.object(power, "IS_WINDOWS", False)
        platform.start()
        self.addCleanup(platform.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cfg = Config(self.root / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.root / "home")
        self.cfg.online["mode"] = "offline"
        self.install = game.GameInstall(self.root / "rt", self.root / "rt" / game.EXE)
        (self.root / "rt").mkdir()

    def setting(self, key):
        return next(s for s in schema.S if s.key == key)

    def plan(self):
        return game.build_plan(self.cfg, self.install, write=False, log=lambda _t: None)

    def test_two_options_in_their_own_section(self):
        for key, label in (("texture_dump", "Dump textures"),
                           ("texture_replace", "Use texture packs")):
            with self.subTest(key=key):
                s = self.setting(key)
                self.assertEqual((s.label, s.group, s.section, s.kind),
                                 (label, "Display", "Texture packs", "bool"))
                self.assertIs(s.default, False)
                self.assertTrue(s.restart)
                self.assertIn(key, crash_report.SETTING_KEYS)
        sections = [s.section for s in schema.S if s.group == "Display"]
        start = sections.index("Texture packs")
        self.assertEqual(sections[start:start + 2], ["Texture packs", "Texture packs"])
        self.assertNotIn("Texture packs", sections[start + 2:])

    def test_off_writes_nothing_and_passes_no_folder(self):
        plan = self.plan()
        self.assertNotIn("texture_", plan.toml_text)
        self.assertFalse(any(a.startswith("--texture_pack_root") for a in plan.argv))

    def test_either_option_reaches_forza_toml_with_the_data_folder(self):
        folder = self.root / "home" / "textures"
        self.assertEqual(game.texture_pack_folder(self.cfg), folder)
        for key in ("texture_dump", "texture_replace"):
            with self.subTest(key=key):
                self.cfg.settings.update(texture_dump=False, texture_replace=False)
                self.cfg.settings[key] = True
                plan = self.plan()
                self.assertEqual(config.parse_toml(plan.toml_text)[key], True)
                self.assertIn(f"--texture_pack_root={folder}", plan.argv)

    def test_a_folder_in_the_extra_arguments_is_the_only_one(self):
        self.cfg.settings["texture_replace"] = True
        self.cfg.advanced["extra_args"] = "--texture_pack_root=/elsewhere"
        argv = self.plan().argv
        self.assertEqual([a for a in argv if a.startswith("--texture_pack_root")],
                         ["--texture_pack_root=/elsewhere"])

    def test_settings_page_has_the_folder_button_and_needs_vulkan(self):
        try:
            from PySide6.QtWidgets import QApplication
        except ImportError:
            self.skipTest("PySide6 is not installed")
        from unittest.mock import MagicMock
        from forza_launcher.ui.settings_page import SettingsPage
        app = QApplication.instance() or QApplication([])
        window = MagicMock()
        window.preview_text.return_value = ""
        page = SettingsPage(self.cfg, window)
        self.addCleanup(page.close)
        self.assertEqual(page.texture_folder_button.text(), "Open textures folder")
        self.assertEqual(page.texture_folder_button.toolTip(), str(self.root / "home" / "textures"))
        self.assertTrue(page.rows["texture_replace"].control.isEnabled())
        self.cfg.settings["gpu_backend"] = "d3d12"
        page._apply_dependencies()
        self.assertFalse(page.rows["texture_replace"].control.isEnabled())
        self.assertTrue(page.rows["texture_dump"].control.isEnabled())
        app.processEvents()


if __name__ == "__main__":
    unittest.main()
