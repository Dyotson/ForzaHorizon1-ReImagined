"""The racing wheel's force feedback strengths (#256) match the SDK's cvars and sit with the others."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import schema
from forza_launcher.config import Config, import_toml, render_toml

PATCH = Path(__file__).resolve().parent.parent / "patches" / "rexglue-sdk" / "0101-wheel-game-force-feedback.patch"
KEYS = ("wheel_ffb_spring_strength", "wheel_rumble_engine_strength", "wheel_rumble_road_strength")


def sdk_int_cvars():
    """name -> (default, low, high) of the integer cvars patch 0101 adds."""
    text = "\n".join(line[1:] for line in PATCH.read_text().splitlines() if line.startswith("+"))
    found = {}
    for m in re.finditer(r"REXCVAR_DEFINE_INT32\((\w+), (\d+),(.*?);", text, re.S):
        name, value, rest = m.groups()
        bounds = re.search(r"\.range\((\d+), (\d+)\)", rest)
        found[name] = (int(value), int(bounds.group(1)), int(bounds.group(2)))
    return found


class WheelForceFeedbackSettings(unittest.TestCase):
    def config(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        return Config(Path(folder.name) / "launcher.json")

    def test_settings_match_the_sdk_cvars(self):
        cvars = sdk_int_cvars()
        for key in KEYS:
            with self.subTest(key=key):
                self.assertIn(key, cvars)
                setting = schema.BY_KEY[key]
                default, lo, hi = cvars[key]
                # 100 percent of the overall strength: the force feedback as before.
                self.assertEqual((setting.default, setting.lo, setting.hi), (default, lo, hi))
                self.assertEqual(default, 100)

    def test_strengths_follow_the_overall_strength(self):
        keys = [s.key for s in schema.for_group("Controls")]
        start = keys.index("wheel_ffb_strength") + 1
        self.assertEqual(tuple(keys[start:start + len(KEYS)]), KEYS)
        self.assertEqual({schema.BY_KEY[k].section for k in KEYS}, {"Racing wheel"})

    def test_defaults_stay_out_of_the_game_configuration_and_values_roundtrip(self):
        cfg = self.config()
        text = render_toml(cfg)
        for key in KEYS:
            self.assertNotIn(key, text)
        expected = dict(wheel_ffb_spring_strength=40, wheel_rumble_engine_strength=25,
                        wheel_rumble_road_strength=90)
        cfg.settings.update(expected)
        text = render_toml(cfg)
        imported = self.config()
        import_toml(imported, text)
        for key, value in expected.items():
            with self.subTest(key=key):
                self.assertIn(f"{key} = {value}", text)
                self.assertEqual(imported.settings[key], value)
        self.assertEqual(schema.coerce(schema.BY_KEY["wheel_ffb_spring_strength"], 150), 100)
        self.assertEqual(schema.coerce(schema.BY_KEY["wheel_rumble_road_strength"], -5), 0)


if __name__ == "__main__":
    unittest.main()
