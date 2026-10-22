"""The racing wheel's clutch and shifter settings match the SDK's cvars and follow the shifter type."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import re
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

PATCH = Path(__file__).resolve().parent.parent / "patches" / "rexglue-sdk" / "0099-wheel-clutch-and-shifter.patch"
KEYS = ("wheel_clutch", "wheel_invert_clutch", "wheel_shifter", "wheel_shifter_buttons",
        "wheel_shifter_clutch", "wheel_clutch_threshold")


def sdk_cvars():
    """name -> (default, allowed values) of the cvars patch 0099 adds."""
    text = "\n".join(line[1:] for line in PATCH.read_text().splitlines() if line.startswith("+"))
    found = {}
    for m in re.finditer(r'REXCVAR_DEFINE_(BOOL|STRING|DOUBLE)\((\w+), ("[^"]*"|[\w.]+),(.*?);', text, re.S):
        kind, name, value, rest = m.groups()
        value = {"BOOL": lambda v: v == "true", "STRING": lambda v: v.strip('"'),
                 "DOUBLE": float}[kind](value)
        allowed = re.search(r"\.allowed\(\{([^}]*)\}\)", rest)
        found[name] = (value, re.findall(r'"([^"]*)"', allowed.group(1)) if allowed else None)
    return found


class WheelShifterSettings(unittest.TestCase):
    def config(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        return Config(Path(folder.name) / "launcher.json")

    def test_settings_match_the_sdk_cvars(self):
        cvars = sdk_cvars()
        self.assertEqual(set(cvars), set(KEYS))
        for key in KEYS:
            with self.subTest(key=key):
                setting = schema.BY_KEY[key]
                default, allowed = cvars[key]
                self.assertEqual(setting.default, default)
                if allowed:
                    self.assertEqual(sorted(v for v, _ in setting.choices), sorted(allowed))

    def test_shifter_section_follows_the_racing_wheel(self):
        keys = [s.key for s in schema.for_group("Controls")]
        start = keys.index("wheel_rumble_style") + 1
        self.assertEqual(tuple(keys[start:start + len(KEYS)]), KEYS)
        self.assertEqual({schema.BY_KEY[k].section for k in KEYS}, {"Shifter and clutch"})
        self.assertTrue(schema.BY_KEY["wheel_shifter"].restart)

    def test_defaults_stay_out_of_the_game_configuration_and_choices_roundtrip(self):
        cfg = self.config()
        text = render_toml(cfg)
        for key in KEYS:
            self.assertNotIn(key, text)
        expected = dict(wheel_clutch=False, wheel_invert_clutch=True, wheel_shifter="h",
                        wheel_shifter_buttons="1=12,2=13,3=14,4=15,5=16,6=17,r=18",
                        wheel_shifter_clutch=True, wheel_clutch_threshold=0.7)
        cfg.settings.update(expected)
        imported = self.config()
        import_toml(imported, render_toml(cfg))
        for key, value in expected.items():
            with self.subTest(key=key):
                self.assertEqual(imported.settings[key], value)
        self.assertEqual(schema.coerce(schema.BY_KEY["wheel_shifter"], "x"), "off")
        self.assertEqual(schema.coerce(schema.BY_KEY["wheel_clutch_threshold"], 5), 1)

    @unittest.skipIf(QApplication is None, "PySide6 is not installed")
    def test_controls_follow_the_shifter_type(self):
        from forza_launcher.ui.settings_page import SettingsPage
        app = QApplication.instance() or QApplication([])
        cfg = self.config()
        window = MagicMock()
        window.preview_text.return_value = ""
        page = SettingsPage(cfg, window)
        self.addCleanup(page.close)

        def enabled(key):
            return page.rows[key].control.isEnabled()

        self.assertFalse(enabled("wheel_shifter_buttons"))
        self.assertFalse(enabled("wheel_shifter_clutch"))
        self.assertFalse(enabled("wheel_clutch_threshold"))
        self.assertTrue(enabled("wheel_invert_clutch"))
        page.set_value("wheel_shifter", "sequential")
        self.assertTrue(enabled("wheel_shifter_buttons"))
        self.assertFalse(enabled("wheel_shifter_clutch"))
        page.set_value("wheel_shifter", "h")
        self.assertTrue(enabled("wheel_shifter_clutch"))
        self.assertFalse(enabled("wheel_clutch_threshold"))
        page.set_value("wheel_shifter_clutch", True)
        self.assertTrue(enabled("wheel_clutch_threshold"))
        page.set_value("wheel_clutch", False)
        self.assertFalse(enabled("wheel_invert_clutch"))
        page.flush()
        app.processEvents()


if __name__ == "__main__":
    unittest.main()
