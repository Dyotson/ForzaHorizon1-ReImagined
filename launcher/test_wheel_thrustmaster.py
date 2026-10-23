"""Thrustmaster wheels (#308): the launcher's table matches the SDK's kThrustmasterModels (patch
0135), and the Bindings rows show a TMX's built-in axes and buttons. Runs alone from the repository
root too: python3 launcher/test_wheel_thrustmaster.py"""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import bindings as b
from forza_launcher import schema

PATCH = Path(__file__).resolve().parent.parent / "patches" / "rexglue-sdk" / \
    "0135-wheel-thrustmaster-profiles.patch"
ACT = {a.key: a for a in b.WHEEL_ACTIONS}


def sdk_models():
    text = "\n".join(line[1:] for line in PATCH.read_text().splitlines() if line.startswith("+"))
    out = []
    for m in re.finditer(r'\{0x([0-9A-F]{4}), "([^"]+)", ([-0-9, ]+)\}', text):
        pid, name, numbers = m.groups()
        out.append(b.ThrustmasterModel(int(pid, 16), name, *[int(n) for n in numbers.split(",")]))
    return out


class Models(unittest.TestCase):
    def test_the_sdks_table(self):
        self.assertEqual(sdk_models(), b.THRUSTMASTER_MODELS)
        self.assertEqual(len(b.THRUSTMASTER_MODELS), 7)

    def test_tmx(self):
        self.assertEqual(b.known_wheel(0x044F, 0xB67F), "Thrustmaster TMX")
        self.assertIsNone(b.known_logitech(0x044F, 0xB67F), "its pedals' rest is read, not assumed")
        self.assertTrue(b.looks_like_wheel("44F B67F", 0x044F, False, 0xB67F),
                        "a wheel by its id: its Windows driver gives it no name")
        self.assertFalse(b.looks_like_wheel("44F B67F", 0x044F, False, 0x1234))
        self.assertEqual(b.default_axes(0x044F, 0xB67F, True),
                         {"steer": 0, "throttle": 2, "brake": 1, "clutch": 3})
        self.assertEqual(b.default_axes(0x044F, 0xB67F, False, 4), b.default_axes(0x044F, 0xB67F, True),
                         "the same on Linux (HID usage order)")
        self.assertEqual(b.default_axes(0x044F, 0xB67F, True, 6), {}, "other axis counts: detected")
        buttons = b.default_buttons(0x044F, 0xB67F)
        self.assertEqual((buttons["a"], buttons["b"], buttons["x"], buttons["y"]), (4, 8, 2, 9))
        self.assertEqual((buttons["lb"], buttons["rb"]), (0, 1), "the paddles shift")
        self.assertEqual(buttons["up"], -1, "the hat is the d-pad")

    def test_rows(self):
        tmx = b.WheelInfo(0x044F, 0xB67F, 4, 13, 1, True)
        s = schema.defaults()
        self.assertEqual(b.wheel_binding_text(ACT["throttle"], s, tmx), "Axis 2 (default)")
        self.assertEqual(b.wheel_binding_text(ACT["rb"], s, tmx), "Button 1 (default)")
        self.assertEqual(b.wheel_binding_text(ACT["a"], s, tmx), "Button 4 (default)")
        t300 = b.WheelInfo(0x044F, 0xB66E, 4, 13, 1, False)
        self.assertEqual(b.wheel_binding_text(ACT["a"], s, t300), "Button 5 (default)")
        self.assertEqual(b.wheel_conflicts(s, tmx), [])


if __name__ == "__main__":
    unittest.main()
