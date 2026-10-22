"""Press-to-bind (#279): binding strings, defaults, conflicts and detection, then the editor
with SDL's virtual joysticks (no hardware)."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import bindings as b
from forza_launcher import schema

ACT = {a.key: a for a in b.WHEEL_ACTIONS}
G29 = b.WheelInfo(0x046D, 0xC24F, 6, 25, 1, False)
G920_WIN = b.WheelInfo(0x046D, 0xC262, 4, 15, 1, True)
OTHER = b.WheelInfo(0x1234, 0x5678, 4, 20, 1, False)


def settings(**values):
    out = schema.defaults()
    out.update(values)
    return out


class StringTests(unittest.TestCase):
    def test_parse_like_the_driver(self):
        # ParseKv: keys lower case without spaces, values read like atoi, no '=' skipped.
        self.assertEqual(b.parse_kv(" Steer =0, THROTTLE=2 ,brake=x,junk,clutch=-1 "),
                         {"steer": 0, "throttle": 2, "brake": 0, "clutch": -1})
        self.assertEqual(b.parse_kv(""), {})

    def test_round_trip_keeps_order_and_unknown_keys(self):
        text = "steer=0,throttle=2,brake=5,clutch=1"
        values = b.parse_kv(text)
        self.assertEqual(b.format_kv(values, b.AXIS_ORDER), text)
        self.assertEqual(b.format_kv(b.parse_kv("zz=3,b=1,a=0"), b.BUTTON_ORDER), "a=0,b=1,zz=3")

    def test_bind_clear_and_reset_write_the_existing_formats(self):
        s = settings(wheel_axes="steer=0,brake=5")
        s.update(b.set_wheel_binding(s, ACT["throttle"], 2))
        self.assertEqual(s["wheel_axes"], "steer=0,throttle=2,brake=5")
        s.update(b.set_wheel_binding(s, ACT["brake"], None))
        self.assertEqual(s["wheel_axes"], f"steer=0,throttle=2,brake={b.NO_AXIS}")
        self.assertEqual(b.wheel_binding_text(ACT["brake"], s, OTHER), "Not bound")
        s.update(b.set_wheel_binding(s, ACT["brake"], "default"))
        self.assertEqual(s["wheel_axes"], "steer=0,throttle=2")
        s.update(b.set_wheel_binding(s, ACT["rb"], 7))
        s.update(b.set_wheel_binding(s, ACT["a"], None))
        self.assertEqual(s["wheel_buttons"], "a=-1,rb=7")
        s.update(b.set_wheel_binding(s, ACT["gearr"], 18))
        s.update(b.set_wheel_binding(s, ACT["gear1"], 12))
        self.assertEqual(s["wheel_shifter_buttons"], "1=12,r=18")
        for key in ("wheel_axes", "wheel_buttons", "wheel_shifter_buttons"):
            self.assertEqual(schema.coerce(schema.BY_KEY[key], s[key]), s[key])


class DefaultTests(unittest.TestCase):
    def test_known_wheels_show_the_drivers_defaults(self):
        s = settings()
        self.assertEqual(b.wheel_binding(ACT["throttle"], s, G29), (2, True))
        self.assertEqual(b.wheel_binding(ACT["throttle"], s, G920_WIN), (1, True))
        self.assertEqual(b.wheel_binding(ACT["b"], s, G29), (2, True))     # circle
        self.assertEqual(b.wheel_binding(ACT["start"], s, G920_WIN), (6, True))
        self.assertEqual(b.wheel_binding(ACT["gearr"], s, G29), (18, True))
        self.assertEqual(b.wheel_binding(ACT["gearr"], s, G920_WIN), (11, True))
        self.assertEqual(b.wheel_binding_text(ACT["gear3"], s, G29), "Button 14 (default)")
        self.assertEqual(b.wheel_binding_text(ACT["steer"], s, OTHER), "Detected at start")
        self.assertEqual(b.wheel_binding_text(ACT["up"], s, OTHER), "Hat (d-pad)")
        self.assertEqual(b.wheel_binding_text(ACT["gear1"], s, OTHER), "Not bound")
        # The driver takes any negative axis number as "the default".
        self.assertEqual(b.wheel_binding(ACT["steer"], settings(wheel_axes="steer=-1"), G29), (0, True))

    def test_wheel_detection_follows_the_driver(self):
        self.assertTrue(b.looks_like_wheel("Logitech G29 Driving Force Racing Wheel"))
        self.assertTrue(b.looks_like_wheel("Some Pad", sdl_wheel=True))
        self.assertFalse(b.looks_like_wheel("Xbox Wireless Racing Wheel", 0x045E, True))
        self.assertFalse(b.looks_like_wheel("Generic USB Joystick"))


class ConflictTests(unittest.TestCase):
    def test_shifter_rows_count_only_in_their_mode(self):
        s = settings(wheel_buttons="rb=12", wheel_shifter_buttons="up=3,down=5")
        self.assertEqual(b.wheel_conflicts(s, G29), [])
        s["wheel_shifter"] = "h"
        self.assertEqual(b.wheel_conflicts(s, G29),
                         [("Button 12", ["Shift up (RB, right paddle)", "Gear 1"])])
        s["wheel_shifter"] = "sequential"
        self.assertEqual(b.wheel_conflicts(s, G29),
                         [("Button 3", ["Rewind (Y)", "Sequential shift up"]),
                          ("Button 5", ["Shift down (LB, left paddle)", "Sequential shift down"])])

    def test_axes_conflict_and_cleared_ones_do_not(self):
        s = settings(wheel_axes="steer=0,throttle=2,brake=2")
        self.assertEqual(b.wheel_conflicts(s, OTHER), [("Axis 2", ["Throttle", "Brake"])])
        s["wheel_axes"] = f"steer=0,throttle=2,brake={b.NO_AXIS}"
        self.assertEqual(b.wheel_conflicts(s, OTHER), [])


class DetectorTests(unittest.TestCase):
    base = b.Snapshot((0, 32767, 32767, -32768), (False, True, False), (0,))

    def test_button_press_and_held_buttons(self):
        d = b.Detector(self.base, "button")
        self.assertIsNone(d.feed(self.base))
        # Button 1 was held when binding started: it counts only after a release.
        self.assertIsNone(d.feed(b.Snapshot(self.base.axes, (False, True, False))))
        self.assertIsNone(d.feed(b.Snapshot(self.base.axes, (False, False, False))))
        self.assertEqual(d.feed(b.Snapshot(self.base.axes, (False, True, False))).number, 1)
        self.assertEqual(b.Detector(self.base, "button").feed(
            b.Snapshot(self.base.axes, (False, True, True))).number, 2)

    def test_axis_direction_rest_and_thresholds(self):
        d = b.Detector(self.base, "axis")
        self.assertIsNone(d.feed(b.Snapshot((9000, 20000, 32767, -32768), (False,) * 3)))
        found = d.feed(b.Snapshot((0, 32767, -5000, -32768), (False,) * 3))
        self.assertEqual((found.kind, found.number, found.direction, found.rest), ("axis", 2, -1, 32767))
        self.assertEqual(found.describe(), "Axis 2, down from the maximum")
        # Buttons do not bind an axis row, and steering needs only a small turn.
        self.assertIsNone(b.Detector(self.base, "axis").feed(b.Snapshot(self.base.axes, (True,) * 3)))
        steer = b.Detector(self.base, "steer").feed(b.Snapshot((6000,) + self.base.axes[1:], (False,) * 3))
        self.assertEqual((steer.number, steer.direction), (0, 1))

    def test_pedal_invert_from_a_bind(self):
        # A G29 pedal rests at the maximum: no invert. Resting at the minimum it needs one.
        self.assertEqual(b.pedal_calibration(32767, -32768, True), (False, 1.0))
        self.assertEqual(b.pedal_calibration(-32768, 32767, True)[0], True)
        # Other wheels: the driver takes the rest from where the axis sat when the game opened it.
        self.assertEqual(b.pedal_calibration(-32768, 32767, False)[0], False)
        self.assertEqual(b.pedal_calibration(32767, -32768, False)[0], False)


try:
    from PySide6.QtWidgets import QApplication
    import sdl2
except ImportError:
    QApplication = None


@unittest.skipIf(QApplication is None, "PySide6 or PySDL2 is not installed")
class EditorTests(unittest.TestCase):
    """The editor on a virtual SDL wheel: bind by moving and pressing, as a player would."""
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from forza_launcher.config import Config
        from forza_launcher.gamepad import Joysticks
        from forza_launcher.ui.controls_page import ControlsPage
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cfg = Config(Path(self.temp.name) / "launcher.json")
        self.backend = Joysticks()
        s = sdl2
        self.index = s.SDL_JoystickAttachVirtual(s.SDL_JOYSTICK_TYPE_WHEEL, 4, 20, 1)
        self.assertGreaterEqual(self.index, 0, s.SDL_GetError())
        self.addCleanup(lambda: s.SDL_JoystickDetachVirtual(self.index))
        self.joy = s.SDL_JoystickOpen(self.index)
        self.addCleanup(lambda: s.SDL_JoystickClose(self.joy))
        self.instance = s.SDL_JoystickInstanceID(self.joy)
        for axis, value in ((0, 0), (1, 32767), (2, 32767), (3, 32767)):
            s.SDL_JoystickSetVirtualAxis(self.joy, axis, value)
        self.page = ControlsPage(self.cfg)
        self.page.bindings.start(self.backend)
        self.addCleanup(self.page.bindings.stop)
        ed = self.page.bindings
        ed.picker.setCurrentIndex(ed.picker.findData(self.instance))
        self.ed = ed
        self.step()

    def step(self, n=2):
        for _ in range(n):
            self.ed.poll()
            self.app.processEvents()

    def row(self, key):
        return next(r for r in self.ed.rows if r.action.key == key)

    def test_bind_throttle_paddle_and_gear_by_pressing_them(self):
        s = sdl2
        self.assertEqual(self.ed.kind_of(self.ed.device), "wheel")
        self.assertEqual(len(self.ed.view.snap.axes), 4)
        self.ed.begin(self.row("throttle"))
        self.assertTrue(self.page.captures(self.instance))
        self.step()
        s.SDL_JoystickSetVirtualAxis(self.joy, 2, -32768)  # press the pedal
        self.step()
        self.assertFalse(self.page.captures(self.instance))
        self.assertEqual(self.cfg.settings["wheel_axes"], "throttle=2")
        self.assertFalse(self.cfg.settings["wheel_invert_throttle"])
        self.assertIn("Axis 2, down from the maximum", self.ed.status.text())
        s.SDL_JoystickSetVirtualAxis(self.joy, 2, 32767)
        self.ed.begin(self.row("rb"))
        s.SDL_JoystickSetVirtualButton(self.joy, 7, 1)
        self.step()
        s.SDL_JoystickSetVirtualButton(self.joy, 7, 0)
        self.assertEqual(self.cfg.settings["wheel_buttons"], "rb=7")
        self.ed.begin(self.row("gear3"))
        s.SDL_JoystickSetVirtualButton(self.joy, 14, 1)
        self.step()
        self.assertEqual(self.cfg.settings["wheel_shifter_buttons"], "3=14")
        self.assertEqual(self.row("gear3").value.text(), "Button 14")
        # A conflict shows once the shifter is on and a pad button shares a gear's button.
        self.cfg.settings["wheel_shifter"] = "h"
        self.ed.begin(self.row("x"))
        s.SDL_JoystickSetVirtualButton(self.joy, 14, 0)
        self.step()
        s.SDL_JoystickSetVirtualButton(self.joy, 14, 1)
        self.step()
        self.assertTrue(self.ed.conflicts.isVisible() or not self.page.isVisible())
        self.assertIn("Button 14", self.ed.conflicts.text())
        self.ed.clear(self.row("x"))
        self.assertEqual(self.cfg.settings["wheel_buttons"], "x=-1,rb=7")
        self.ed.reset(self.row("x"))
        self.assertEqual(self.cfg.settings["wheel_buttons"], "rb=7")
        self.assertEqual(self.ed.conflicts.text() if self.ed.conflicts.isVisible() else "", "")

    def test_steering_direction_sets_the_invert_switch(self):
        self.ed.begin(self.row("steer"))
        sdl2.SDL_JoystickSetVirtualAxis(self.joy, 0, -12000)  # turned left when asked for right
        self.step()
        self.assertEqual(self.cfg.settings["wheel_axes"], "steer=0")
        self.assertTrue(self.cfg.settings["wheel_invert_steering"])

    def test_timeout_escape_and_unplug_cancel(self):
        from forza_launcher.ui import bindings_editor
        self.ed.begin(self.row("brake"))
        self.ed.binding = (self.ed.binding[0], self.ed.binding[1], 0)
        self.step()
        self.assertIsNone(self.ed.binding)
        self.assertIn("Nothing was pressed", self.ed.status.text())
        self.ed.begin(self.row("brake"))
        self.ed.cancel("Binding cancelled.")
        self.assertEqual(self.cfg.settings["wheel_axes"], "")
        self.ed.begin(self.row("brake"))
        sdl2.SDL_JoystickDetachVirtual(self.index)
        self.step()
        self.assertIsNone(self.ed.binding)
        self.assertFalse(self.page.captures(self.instance))
        self.assertEqual(bindings_editor.BIND_TIMEOUT, 10.0)

    def test_navigation_ignores_the_device_while_binding(self):
        from forza_launcher.gamepad import State
        from forza_launcher.ui.gamepad import Navigation
        window = Mock()
        window.controls = self.page
        window.running.return_value = False
        nav = Navigation.__new__(Navigation)
        nav.window, nav.backend = window, Mock()
        from forza_launcher.gamepad import Actions
        nav.actions = Actions()
        nav.used = False
        nav.update_action_icon = nav.update_ring = lambda *_: None
        nav.active_scope = lambda: window
        delivered = []
        nav.deliver = lambda action, scope: delivered.append(action)
        from PySide6.QtCore import QTimer
        nav.backend.read.return_value = State(self.instance)
        nav.poll()
        self.ed.begin(self.row("a"))
        nav.backend.read.return_value = State(self.instance, frozenset({"accept"}))
        orig = QTimer.singleShot
        try:
            QTimer.singleShot = staticmethod(lambda _ms, fn: fn())
            nav.poll()
            self.assertEqual(delivered, [])
            self.ed.cancel("")
            nav.poll()  # still held after the bind: navigation waits for a neutral pad
            self.assertEqual(delivered, [])
            nav.backend.read.return_value = State(self.instance)
            nav.poll()
            nav.backend.read.return_value = State(self.instance, frozenset({"accept"}))
            nav.poll()
            self.assertEqual(delivered, ["accept"])
        finally:
            QTimer.singleShot = orig


if __name__ == "__main__":
    unittest.main()
