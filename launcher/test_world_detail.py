from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import config, crash_report, schema
from forza_launcher.config import Config


class WorldDetailTests(unittest.TestCase):
    """Settings > Display > World detail (forza-recomp/src/hooks/world_detail.cpp)."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cfg = Config(self.root / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.root)

    def setting(self, key):
        return next(s for s in schema.S if s.key == key)

    def test_options_default_to_the_console_and_offer_three_levels(self):
        for key, label in (("world_shadow_detail", "Shadow detail"),
                           ("world_texture_detail", "Distant texture detail")):
            with self.subTest(key=key):
                s = self.setting(key)
                self.assertEqual((s.label, s.group, s.section), (label, "Display", "World detail"))
                self.assertEqual(s.default, "console")
                self.assertEqual([v for v, _ in s.choices], ["console", "high", "ultra"])
                self.assertTrue(s.restart)
                self.assertIn(key, crash_report.SETTING_KEYS)

    def test_console_writes_nothing_and_other_levels_reach_forza_toml(self):
        text = config.render_toml(self.cfg)
        self.assertNotIn("world_shadow_detail", text)
        self.assertNotIn("world_texture_detail", text)
        self.cfg.settings.update(world_shadow_detail="ultra", world_texture_detail="high")
        values = config.parse_toml(config.render_toml(self.cfg))
        self.assertEqual(values["world_shadow_detail"], "ultra")
        self.assertEqual(values["world_texture_detail"], "high")

    def test_unknown_values_fall_back_to_the_console(self):
        for bad in ("Ultra", "max", 3, True):
            with self.subTest(bad=bad):
                self.assertEqual(schema.coerce(self.setting("world_shadow_detail"), bad), "console")

    def test_the_section_is_one_card_after_picture(self):
        sections = [s.section for s in schema.S if s.group == "Display"]
        start = sections.index("World detail")
        self.assertEqual(sections[start:start + 2], ["World detail", "World detail"])
        self.assertNotIn("World detail", sections[start + 2:])
        self.assertNotIn("Picture", sections[start:])


if __name__ == "__main__":
    unittest.main()
