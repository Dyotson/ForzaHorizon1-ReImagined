"""Pedals and shifters on their own USB device (#272): the settings match the SDK's cvars (patch
0128), the known devices and the picking follow the driver, the bindings write the device
settings, the Bindings editor binds a pedal on a pedal device and a gear on a shifter device with
SDL's virtual joysticks (no hardware), and on Linux the devices SDL2 leaves out are read from
evdev (a uinput pedal set, when /dev/uinput is writable). Runs alone from the repository root too:
python3 launcher/test_wheel_devices.py"""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import ctypes
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import bindings as b
from forza_launcher import schema
from forza_launcher.config import Config, import_toml, render_toml
from forza_launcher.gamepad import DeviceInfo

try:
    from PySide6.QtWidgets import QApplication
    import sdl2
except ImportError:
    QApplication = None

PATCH = Path(__file__).resolve().parent.parent / "patches" / "rexglue-sdk" / \
    "0128-wheel-pedal-and-shifter-devices.patch"
KEYS = ("wheel_pedal_device", "wheel_pedal_axes", "wheel_shifter_device",
        "wheel_shifter_device_buttons")
ACT = {a.key: a for a in b.WHEEL_ACTIONS}
WHEEL = b.WheelInfo(0x3670, 0x0502, 4, 20, 1, False)
P1000 = b.AuxInfo("SIMAGIC P1000", 0x0483, 0x0525, 3, 0)
DS8X = b.AuxInfo("SIMAGIC DS-8X", 0x0483, 0x0531, 0, 11)
AUX = {"pedals": P1000, "shifter": DS8X}


def added():
    return "\n".join(line[1:] for line in PATCH.read_text().splitlines() if line.startswith("+"))


def settings(**values):
    out = schema.defaults()
    out.update(values)
    return out


def dev(name, vid, pid, instance=0, sdl_wheel=False, guid=""):
    return DeviceInfo(instance, name, guid, vid, pid, False, sdl_wheel)


class SettingsMatchTheSdk(unittest.TestCase):
    def test_cvars(self):
        found = {}
        for m in re.finditer(r'REXCVAR_DEFINE_STRING\((\w+), "([^"]*)",(.*?);', added(), re.S):
            name, value, rest = m.groups()
            allowed = re.search(r"\.allowed\(\{([^}]*)\}\)", rest)
            found[name] = (value, re.findall(r'"([^"]*)"', allowed.group(1)) if allowed else None)
        for key in KEYS:
            with self.subTest(key=key):
                default, allowed = found[key]
                setting = schema.BY_KEY[key]
                self.assertEqual(setting.default, default)
                if allowed:
                    self.assertEqual(sorted(v for v, _ in setting.choices), sorted(allowed))
        self.assertEqual(schema.BY_KEY["wheel_pedal_device"].section, "Racing wheel")
        self.assertEqual(schema.BY_KEY["wheel_shifter_device"].section, "Shifter and clutch")

    def test_models_are_the_drivers(self):
        role = {"kPedals": "pedals", "kShifter": "shifter", "kOther": "other"}
        sdk = []
        for m in re.finditer(r'\{0x([0-9A-F]{4}), 0x([0-9A-F]{4}), AuxRole::(\w+), "([^"]*)"([^}]*)\}',
                             added()):
            vid, pid, r, name, rest = m.groups()
            nums = [int(n) for n in re.findall(r"-?\d+", rest)]
            sdk.append(b.AuxModel(int(vid, 16), int(pid, 16), role[r], name, *nums))
        self.assertEqual(len(sdk), len(b.AUX_MODELS))
        self.assertEqual(sdk, b.AUX_MODELS)

    def test_defaults_stay_out_and_values_roundtrip(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        cfg = Config(Path(folder.name) / "launcher.json")
        text = render_toml(cfg)
        for key in KEYS:
            self.assertNotIn(key, text)
        expected = dict(wheel_pedal_device="SIMAGIC P1000",
                        wheel_pedal_axes="throttle=2,brake=1,clutch=0",
                        wheel_shifter_device="0483:0531", wheel_shifter_device_buttons="1=0,r=8")
        cfg.settings.update(expected)
        imported = Config(Path(folder.name) / "other.json")
        import_toml(imported, render_toml(cfg))
        for key, value in expected.items():
            with self.subTest(key=key):
                self.assertEqual(imported.settings[key], value)


class Detection(unittest.TestCase):
    def test_roles(self):
        self.assertEqual(b.detect_aux_role(0x0483, 0x0525, ""), "pedals")
        self.assertEqual(b.detect_aux_role(0x0EB7, 0x1A92, ""), "shifter")
        self.assertEqual(b.detect_aux_role(0x0EB7, 0x1A93, ""), "other")
        self.assertEqual(b.detect_aux_role(0x1, 0x2, "Thrustmaster TH8A Shifter"), "shifter")
        self.assertEqual(b.detect_aux_role(0x346E, 0x9, "Gudsen MOZA SR-P Pedals"), "pedals")
        self.assertIsNone(b.detect_aux_role(0x346E, 0x9, "Gudsen MOZA R5 Racing Wheel and Pedals"))
        self.assertIsNone(b.detect_aux_role(0x3670, 0x0502, "SIMAGIC EVO PRO"))

    def test_pick_like_the_driver(self):
        evo = dev("SIMAGIC EVO PRO", 0x3670, 0x0502, sdl_wheel=True)
        p1000 = dev("SIMAGIC P1000", 0x0483, 0x0525)
        ds8x = dev("SIMAGIC DS-8X", 0x0483, 0x0531)
        box = dev("Arduino Leonardo", 0x2341, 0x8036)
        pick = b.pick_aux([evo, p1000, ds8x], settings())
        self.assertEqual((pick.pedals, pick.shifter), (1, 2))
        pick = b.pick_aux([evo, p1000, ds8x], settings(wheel_pedal_device="none",
                                                       wheel_shifter_device="none"))
        self.assertEqual((pick.pedals, pick.shifter, pick.other), (None, None, (1, 2)))
        pick = b.pick_aux([evo, box, p1000], settings(wheel_shifter_device="Arduino Leonardo"))
        self.assertEqual((pick.pedals, pick.shifter), (2, 1))
        pick = b.pick_aux([evo, p1000], settings(wheel_pedal_device="simagic"))
        self.assertEqual(pick.pedals, 1)  # the part of a name prefers the device that is no base
        pick = b.pick_aux([evo, p1000], settings(wheel_pedal_device="0483:0525"))
        self.assertEqual(pick.pedals, 1)


class Bindings(unittest.TestCase):
    def test_rows_on_the_devices_and_their_defaults(self):
        s = settings()
        self.assertEqual(b.wheel_binding_text(ACT["throttle"], s, WHEEL, AUX),
                         "Axis 2 on SIMAGIC P1000 (default)")
        self.assertEqual(b.wheel_binding_text(ACT["clutch"], s, WHEEL, AUX),
                         "Axis 0 on SIMAGIC P1000 (default)")
        self.assertEqual(b.wheel_binding_text(ACT["gear1"], s, WHEEL, AUX),
                         "Button 0 on SIMAGIC DS-8X (default)")
        self.assertEqual(b.wheel_binding_text(ACT["gearr"], s, WHEEL, AUX),
                         "Button 8 on SIMAGIC DS-8X (default)")
        # Unknown pedals: detected when the game starts; without devices: the wheel, as before.
        other = {"pedals": b.AuxInfo("Some Pedals", 1, 2, 4, 0)}
        self.assertEqual(b.wheel_binding_text(ACT["throttle"], s, WHEEL, other),
                         "Detected at start on Some Pedals")
        self.assertEqual(b.wheel_binding_text(ACT["throttle"], s, WHEEL, {}), "Detected at start")
        self.assertEqual(b.wheel_binding_text(ACT["gear1"], s, WHEEL, {}), "Not bound")
        # wheel_axes keeps a pedal on the wheel; wheel_pedal_axes puts it on the pedal device even
        # while that is unplugged.
        s = settings(wheel_axes="brake=3", wheel_pedal_axes="clutch=1",
                     wheel_pedal_device="SIMAGIC P1000")
        self.assertEqual(b.wheel_binding_text(ACT["brake"], s, WHEEL, AUX), "Axis 3")
        self.assertEqual(b.wheel_binding_text(ACT["clutch"], s, WHEEL, {}), "Axis 1 on SIMAGIC P1000")

    def test_bind_on_a_device_clear_reset(self):
        s = settings()
        ch = b.set_aux_binding(s, ACT["throttle"], "SIMAGIC P1000", 2)
        self.assertEqual(ch, {"wheel_pedal_device": "SIMAGIC P1000", "wheel_pedal_axes": "throttle=2"})
        s.update(ch)
        s.update(b.set_aux_binding(s, ACT["brake"], "SIMAGIC P1000", 1))
        self.assertEqual(s["wheel_pedal_axes"], "throttle=2,brake=1")
        # Another pedal device: the first one's numbers go.
        ch = b.set_aux_binding(s, ACT["clutch"], "Heusinkveld Sprint", 2)
        self.assertEqual(ch, {"wheel_pedal_device": "Heusinkveld Sprint", "wheel_pedal_axes": "clutch=2"})
        s.update(ch)
        s.update(b.set_aux_binding(s, ACT["clutch"], None, None))  # clear on its device
        self.assertEqual(s["wheel_pedal_axes"], "clutch=99")
        self.assertEqual(b.wheel_binding_text(ACT["clutch"], s, WHEEL, {}),
                         "Not bound on Heusinkveld Sprint")
        s.update(b.reset_row(s, ACT["clutch"]))
        self.assertEqual(s["wheel_pedal_axes"], "")
        # Bound on the wheel: off the pedal device's axes.
        s.update(b.set_aux_binding(s, ACT["throttle"], "Heusinkveld Sprint", 0))
        ch = b.set_row_on_wheel(s, ACT["throttle"], 2, AUX)
        self.assertEqual(ch, {"wheel_axes": "throttle=2", "wheel_pedal_axes": ""})
        # A wheel_axes number goes when the pedal device takes the pedal.
        s.update(ch)
        ch = b.set_aux_binding(s, ACT["throttle"], "Heusinkveld Sprint", 1)
        self.assertEqual(ch["wheel_axes"], "")
        # Gears: on the shifter device; a gear bound on the wheel reads the shifter from the wheel.
        s = settings()
        ch = b.set_aux_binding(s, ACT["gear1"], "SIMAGIC DS-8X", 3)
        self.assertEqual(ch, {"wheel_shifter_device": "SIMAGIC DS-8X",
                              "wheel_shifter_device_buttons": "1=3"})
        s.update(ch)
        ch = b.set_row_on_wheel(s, ACT["gear2"], 13, AUX)
        self.assertEqual(ch, {"wheel_shifter_buttons": "2=13", "wheel_shifter_device": "none"})
        self.assertEqual(set(b.RESET_ALL_WHEEL), {"wheel_axes", "wheel_buttons",
                                                  "wheel_shifter_buttons", *KEYS,
                                                  "wheel_handbrake_device", "wheel_handbrake_axis",
                                                  "wheel_invert_handbrake"})

    def test_conflicts_per_device(self):
        # Button 0 is A on the wheel and reverse on the DS-8X: no conflict, other devices; gears 1
        # and 2 on the DS-8X's button 9 are one.
        s = settings(wheel_shifter="h", wheel_shifter_device_buttons="1=9,2=9,r=0")
        found = dict(b.wheel_conflicts(s, WHEEL, AUX))
        self.assertEqual(found, {"Button 9 on SIMAGIC DS-8X": ["Gear 1", "Gear 2"]})

    def test_merge_and_split(self):
        parts = [b.Part(1, "wheel", 2, 3), b.Part(2, "pedals", 3, 0), b.Part(3, "shifter", 0, 4)]
        snap = b.merge(parts, [b.Snapshot((1, 2), (False, True, False), (0,)),
                               b.Snapshot((7, 8, 9)), b.Snapshot((), (True,))])
        self.assertEqual(snap.axes, (1, 2, 7, 8, 9))
        self.assertEqual(snap.buttons, (False, True, False, True, False, False, False))
        self.assertEqual(b.split(parts, "axis", 3), (1, 1))
        self.assertEqual(b.split(parts, "button", 3), (2, 0))
        self.assertEqual(b.split(parts, "axis", 9), (None, None))

    def test_calibration_finds_the_pedals_on_their_device(self):
        parts = [b.Part(1, "SIMAGIC EVO PRO", 2, 0), b.Part(2, "SIMAGIC P1000", 3, 0)]
        cal = b.Calibration(False, parts)

        def snap(steer, *pedals):
            return b.Snapshot((steer, 0) + pedals)
        rest = (-32768, -32768, -32768)
        for s, step in ((snap(0, *rest), "centre"), (snap(-32768, *rest), "left"),
                        (snap(32767, *rest), "right"), (snap(round(32767 * 90 / 450), *rest), "quarter")):
            cal.feed(s)
            self.assertTrue(cal.next(), step)
        self.assertEqual(cal.steer, 0)
        for pressed, name in ((4, "throttle"), (3, "brake"), (2, "clutch")):
            axes = [0, 0, -32768, -32768, -32768]
            axes[pressed] = 32767
            cal.feed(b.Snapshot(tuple(axes)))
            cal.feed(snap(0, *rest))
            self.assertTrue(cal.next(), name)
        lines, _ = cal.results(settings())
        self.assertIn("Throttle: axis 2 on SIMAGIC P1000, rests at the minimum", lines)
        out = cal.changes(settings(wheel_axes="throttle=1"))
        self.assertEqual(out["wheel_pedal_device"], "SIMAGIC P1000")
        self.assertEqual(out["wheel_pedal_axes"], "throttle=2,brake=1,clutch=0")
        self.assertEqual(out["wheel_axes"], "steer=0")
        self.assertFalse(out["wheel_invert_throttle"])


class Evdev(unittest.TestCase):
    """SDL3's numbering of an evdev device, for the devices SDL2 leaves out on Linux."""
    def test_numbers_like_sdl3(self):
        from forza_launcher import evdev_joysticks as ej
        # Rx, Ry, Rz and a hat pair: the hat is no axis.
        self.assertEqual(ej.axis_numbers({0x05, 0x03, 0x04, 0x10, 0x11}), [0x03, 0x04, 0x05])
        # BTN_TRIGGER (0x120) upwards first, then the codes below it (BTN_0 is 0x100).
        self.assertEqual(ej.button_numbers({0x100, 0x121, 0x120, 0x2C0}), [0x120, 0x121, 0x2C0, 0x100])
        self.assertEqual((ej.scale(0, 0, 65535), ej.scale(65535, 0, 65535), ej.scale(255, 0, 255)),
                         (-32768, 32767, 32767))
        self.assertEqual(ej.scale(-512, -512, 511), -32768)
        self.assertTrue(ej.looks_like_joystick({0x03}, set()))
        self.assertTrue(ej.looks_like_joystick(set(), {0x120}))
        self.assertFalse(ej.looks_like_joystick(set(), {0x1E}))  # a keyboard's A

    @unittest.skipUnless(sys.platform.startswith("linux") and os.access("/dev/uinput", os.W_OK),
                         "Linux with a writable /dev/uinput")
    def test_pedals_sdl2_leaves_out(self):
        try:
            from evdev import AbsInfo, UInput, ecodes as e
        except ImportError:
            self.skipTest("python-evdev is not installed")
        from forza_launcher.gamepad import Joysticks
        info = AbsInfo(value=0, min=0, max=65535, fuzz=0, flat=0, resolution=0)
        ui = UInput({e.EV_ABS: [(e.ABS_RX, info), (e.ABS_RY, info), (e.ABS_RZ, info)]},
                    name="Test Sim Pedals 272", vendor=0x0483, product=0x0525, bustype=e.BUS_USB)
        self.addCleanup(ui.close)
        import time
        backend = None
        try:
            backend = Joysticks()
        except (ImportError, OSError, RuntimeError) as exc:
            self.skipTest(f"SDL2: {exc}")
        self.addCleanup(backend.close)
        for _ in range(40):  # udev tags the new node
            found = [d for d in backend.devices() if d.name == "Test Sim Pedals 272"]
            if found:
                break
            time.sleep(0.05)
            backend.evdev_nodes = None
        self.assertEqual(len(found), 1)
        pedals = found[0]
        self.assertLess(pedals.instance, 0)
        self.assertEqual(b.detect_aux_role(pedals.vid, pedals.pid, pedals.name), "pedals")
        backend.watch([pedals.instance])
        self.assertEqual(backend.extra_counts(pedals.instance), (3, 0, 0))
        self.assertEqual(backend.read_extra(pedals.instance).axes, (-32768, -32768, -32768))
        ui.write(e.EV_ABS, e.ABS_RZ, 65535)
        ui.syn()
        time.sleep(0.05)
        self.assertEqual(backend.read_extra(pedals.instance).axes, (-32768, -32768, 32767))


class VirtualDevice:
    """An SDL virtual joystick with its own name, vendor and product."""
    def __init__(self, name, vid, pid, axes, buttons, hats=0, kind=None):
        s = sdl2
        self.name = name.encode()
        desc = s.SDL_VirtualJoystickDesc()
        desc.version = s.SDL_VIRTUAL_JOYSTICK_DESC_VERSION
        desc.type = kind if kind is not None else s.SDL_JOYSTICK_TYPE_UNKNOWN
        desc.naxes, desc.nbuttons, desc.nhats = axes, buttons, hats
        desc.vendor_id, desc.product_id = vid, pid
        desc.name = self.name
        self.index = s.SDL_JoystickAttachVirtualEx(ctypes.byref(desc))
        if self.index < 0:
            raise RuntimeError(s.SDL_GetError())
        self.joy = s.SDL_JoystickOpen(self.index)
        self.instance = s.SDL_JoystickInstanceID(self.joy)
        self.attached = True

    def axis(self, n, value):
        sdl2.SDL_JoystickSetVirtualAxis(self.joy, n, value)

    def button(self, n, down):
        sdl2.SDL_JoystickSetVirtualButton(self.joy, n, 1 if down else 0)

    def detach(self):
        if self.attached:
            index = self.device_index()
            sdl2.SDL_JoystickClose(self.joy)
            if index >= 0:
                sdl2.SDL_JoystickDetachVirtual(index)
            self.attached = False

    def device_index(self):
        for i in range(sdl2.SDL_NumJoysticks()):
            if sdl2.SDL_JoystickGetDeviceInstanceID(i) == self.instance:
                return i
        return -1


def three_devices(test):
    """A Simagic EVO Pro, P1000 pedals and a DS-8X shifter, attached and set at rest."""
    wheel = VirtualDevice("SIMAGIC EVO PRO", 0x3670, 0x0502, 4, 20, 1, sdl2.SDL_JOYSTICK_TYPE_WHEEL)
    pedals = VirtualDevice("SIMAGIC P1000", 0x0483, 0x0525, 3, 0)
    shifter = VirtualDevice("SIMAGIC DS-8X", 0x0483, 0x0531, 0, 11)
    for d in (shifter, pedals, wheel):
        test.addCleanup(d.detach)
    for n, v in ((0, 0), (1, 32767), (2, 32767), (3, 32767)):
        wheel.axis(n, v)
    for n in range(3):
        pedals.axis(n, -32768)
    return wheel, pedals, shifter


@unittest.skipIf(QApplication is None, "PySide6 or PySDL2 is not installed")
class EditorWithThreeDevices(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from forza_launcher.gamepad import Joysticks
        from forza_launcher.ui.controls_page import ControlsPage
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cfg = Config(Path(self.temp.name) / "launcher.json")
        self.backend = Joysticks()
        self.wheel, self.pedals, self.shifter = three_devices(self)
        self.page = ControlsPage(self.cfg)
        self.page.bindings.start(self.backend)
        self.addCleanup(self.page.bindings.stop)
        self.ed = self.page.bindings
        self.ed.picker.setCurrentIndex(self.ed.picker.findData(self.wheel.instance))
        self.step()

    def step(self, n=2):
        for _ in range(n):
            self.ed.poll()
            self.app.processEvents()

    def row(self, key):
        return next(r for r in self.ed.rows if r.action.key == key)

    def label(self, device):
        return self.ed.picker.itemText(self.ed.picker.findData(device.instance))

    def test_picker_and_default_rows(self):
        self.assertEqual(self.label(self.wheel), "SIMAGIC EVO PRO (wheel)")
        self.assertEqual(self.label(self.pedals), "SIMAGIC P1000 (pedals, read with the wheel)")
        self.assertEqual(self.label(self.shifter), "SIMAGIC DS-8X (shifter, read with the wheel)")
        self.assertEqual(self.row("throttle").value.text(), "Axis 2 on SIMAGIC P1000 (default)")
        self.assertEqual(self.row("gear1").value.text(), "Button 0 on SIMAGIC DS-8X (default)")
        self.assertEqual(self.row("steer").value.text(), "Detected at start")
        # The pedal device on its own: no rows, a pointer to the wheel.
        self.ed.picker.setCurrentIndex(self.ed.picker.findData(self.pedals.instance))
        self.step()
        self.assertEqual(self.ed.rows, [])
        self.assertIn("read together with the wheel", self.ed.intro.text())

    def test_bind_a_pedal_and_a_gear_on_their_devices(self):
        ed, st = self.ed, self.cfg.settings
        ed.begin(self.row("throttle"))
        self.step()
        self.pedals.axis(1, 32767)  # press the pedal on Ry
        self.step()
        self.assertIsNone(ed.binding)
        self.assertEqual((st["wheel_pedal_device"], st["wheel_pedal_axes"]),
                         ("SIMAGIC P1000", "throttle=1"))
        self.assertFalse(st["wheel_invert_throttle"])
        self.assertIn("on SIMAGIC P1000", ed.status.text())
        self.assertEqual(self.row("throttle").value.text(), "Axis 1 on SIMAGIC P1000")
        self.pedals.axis(1, -32768)
        ed.begin(self.row("gear1"))
        self.step()
        self.shifter.button(3, True)
        self.step()
        self.shifter.button(3, False)
        self.assertEqual((st["wheel_shifter_device"], st["wheel_shifter_device_buttons"]),
                         ("SIMAGIC DS-8X", "1=3"))
        self.assertEqual(self.row("gear1").value.text(), "Button 3 on SIMAGIC DS-8X")
        # The brake on the wheel's own axis stays on the wheel.
        ed.begin(self.row("brake"))
        self.step()
        self.wheel.axis(3, -32768)
        self.step()
        self.wheel.axis(3, 32767)
        self.step()
        self.assertEqual(st["wheel_axes"], "brake=3")
        self.assertEqual(self.row("brake").value.text(), "Axis 3")
        # Steering is the wheel's: a pedal does not bind it.
        ed.begin(self.row("steer"))
        self.step()
        self.pedals.axis(0, 32767)
        self.step()
        self.assertIsNotNone(ed.binding)
        self.wheel.axis(0, 20000)
        self.step()
        self.assertIsNone(ed.binding)
        self.assertEqual(st["wheel_axes"], "steer=0,brake=3")
        self.pedals.axis(0, -32768)
        # A button row is the wheel's too.
        ed.begin(self.row("x"))
        self.step()
        self.shifter.button(6, True)
        self.step()
        self.assertIsNotNone(ed.binding)
        ed.cancel("")
        self.shifter.button(6, False)
        # Clear and reset on the device.
        ed.clear(self.row("throttle"))
        self.assertEqual(st["wheel_pedal_axes"], "throttle=99")
        self.assertEqual(self.row("throttle").value.text(), "Not bound on SIMAGIC P1000")
        ed.reset(self.row("throttle"))
        self.assertEqual(self.row("throttle").value.text(), "Axis 2 on SIMAGIC P1000 (default)")
        ed.reset_everything()
        self.assertEqual({k: st[k] for k in b.RESET_ALL_WHEEL}, b.RESET_ALL_WHEEL)

    def test_unplugged_pedals_during_a_bind(self):
        ed = self.ed
        ed.begin(self.row("clutch"))
        self.step()
        self.pedals.detach()
        self.step()
        self.assertIsNotNone(ed.binding)  # still waiting: the wheel's own pedals can bind it
        self.wheel.axis(1, -32768)
        self.step()
        self.assertEqual(self.cfg.settings["wheel_axes"], "clutch=1")
        ed.scan(force=True)
        self.step()
        self.assertEqual(self.row("throttle").value.text(), "Detected at start")

    def test_calibration_with_the_pedal_device(self):
        ed = self.ed
        ed.begin_calibration()
        panel = ed.calibration

        def at(steer=0, pedal=None):
            self.wheel.axis(0, steer)
            for n in range(3):
                self.pedals.axis(n, 32767 if n == pedal else -32768)
            self.step()
        at()
        panel.go_next()
        at(-32768)
        panel.go_next()
        at(32767)
        panel.go_next()
        at(round(32767 * 90 / 450))
        panel.go_next()
        for pedal in (2, 1, 0):  # throttle Rz, brake Ry, clutch Rx
            at(0, pedal)
            at()
            panel.go_next()
        self.assertTrue(panel.cal.done)
        panel.go_apply()
        st = self.cfg.settings
        self.assertEqual(st["wheel_pedal_device"], "SIMAGIC P1000")
        self.assertEqual(st["wheel_pedal_axes"], "throttle=2,brake=1,clutch=0")
        self.assertEqual(st["wheel_axes"], "steer=0")


if __name__ == "__main__":
    unittest.main()
