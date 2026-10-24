"""Linux joysticks SDL2 leaves out, read straight from evdev for the Bindings editor (#272).

The launcher reads controllers through SDL2, which on Linux takes an input device for a joystick
only when it has an X and a Y axis. Pedal sets (Simagic, Heusinkveld and Moza pedals report Rx, Ry
and Rz; some only X, Y, Z without buttons) and shifters (buttons only) are then missing, although
udev marks them as joysticks and the game's SDL3 reads them. This lists those devices and reads
their state with the numbers SDL3 gives them: axes in event code order without the hats, scaled
from their range to -32768..32767 (SDL's AxisCorrect without dead zones), and buttons from
BTN_JOYSTICK upwards first, then the codes below it (SDL's ConfigJoystick).
"""
import math
import os
import re
import struct
from pathlib import Path

EV_KEY, EV_ABS = 0x01, 0x03
ABS_HAT0X, ABS_HAT3Y, ABS_CNT = 0x10, 0x17, 0x40
BTN_JOYSTICK, KEY_CNT = 0x120, 0x300
# What makes a joystick for SDL3 and udev: joystick or gamepad buttons, or one of these axes.
JOYSTICK_AXES = (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A)
JOYSTICK_BUTTONS = tuple(range(0x120, 0x140)) + tuple(range(0x2C0, 0x2E8))
INSTANCE_BASE = -1000  # instance ids of these devices: -1000 - event number (SDL's are >= 0)


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("E") << 8) | nr


def _read(fd, nr, size):
    buf = bytearray(size)
    import fcntl  # Linux only: the numbering helpers above stay importable on Windows and macOS
    fcntl.ioctl(fd, _ioc(2, nr, size), buf)
    return bytes(buf)


def _bits(data):
    return {i for i in range(len(data) * 8) if data[i // 8] >> (i % 8) & 1}


def axis_numbers(abs_codes):
    """The event codes of the axes in SDL's numbering (hats are no axes)."""
    return [c for c in sorted(abs_codes) if not ABS_HAT0X <= c <= ABS_HAT3Y]


def button_numbers(key_codes):
    """The event codes of the buttons in SDL's numbering."""
    keys = sorted(key_codes)
    return [c for c in keys if c >= BTN_JOYSTICK] + [c for c in keys if c < BTN_JOYSTICK]


def scale(value, lo, hi):
    """An axis value as SDL reports it: its range mapped onto -32768..32767."""
    if hi == lo:
        return value
    v = math.floor((value - lo) * 65535.0 / (hi - lo) - 32768 + 0.5)
    return max(-32768, min(32767, v))


def looks_like_joystick(abs_codes, key_codes):
    return any(c in abs_codes for c in JOYSTICK_AXES) or any(c in key_codes for c in JOYSTICK_BUTTONS)


def _udev_joystick(path):
    """udev's ID_INPUT_JOYSTICK for an event node; None when there is no udev database."""
    try:
        st = os.stat(path)
        data = Path(f"/run/udev/data/c{os.major(st.st_rdev)}:{os.minor(st.st_rdev)}").read_text()
    except OSError:
        return None
    return "E:ID_INPUT_JOYSTICK=1" in data


class EvdevJoystick:
    """One open event node and what SDL3 would number in it."""
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        try:
            self.name = _read(self.fd, 0x06, 256).split(b"\0", 1)[0].decode("utf-8", "replace")
            _bus, self.vid, self.pid, _version = struct.unpack("4H", _read(self.fd, 0x02, 8))
            ev = _bits(_read(self.fd, 0x20, 4))
            abs_codes = _bits(_read(self.fd, 0x20 + EV_ABS, ABS_CNT // 8)) if EV_ABS in ev else set()
            key_codes = _bits(_read(self.fd, 0x20 + EV_KEY, KEY_CNT // 8)) if EV_KEY in ev else set()
            self.key_codes_all = key_codes
            self.abs_codes_all = abs_codes
            self.axes = axis_numbers(abs_codes)
            self.buttons = button_numbers(key_codes)
            self.ranges = {}
            for code in self.axes:
                _value, lo, hi, *_ = struct.unpack("6i", _read(self.fd, 0x40 + code, 24))
                self.ranges[code] = (lo, hi)
        except OSError:
            os.close(self.fd)
            raise

    @property
    def counts(self):
        return len(self.axes), len(self.buttons), 0

    def read(self):
        """(axes, buttons) now, or None when the device is gone."""
        try:
            axes = []
            for code in self.axes:
                value = struct.unpack("6i", _read(self.fd, 0x40 + code, 24))[0]
                axes.append(scale(value, *self.ranges[code]))
            keys = _bits(_read(self.fd, 0x18, KEY_CNT // 8)) if self.buttons else set()
        except OSError:
            return None
        return tuple(axes), tuple(c in keys for c in self.buttons)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def scan(listed=()):
    """{event number: EvdevJoystick} of the joysticks SDL2 leaves out: udev joysticks (or, without
    udev, devices with joystick axes or buttons) whose (name, vid, pid) is not in `listed`, which
    holds those SDL2 has. The caller closes them."""
    taken = list(listed)
    out = {}
    nodes = sorted(Path("/dev/input").glob("event*"),
                   key=lambda p: int(re.sub(r"\D", "", p.name) or 0))
    for node in nodes:
        udev = _udev_joystick(node)
        if udev is False:
            continue
        try:
            joy = EvdevJoystick(str(node))
        except OSError:
            continue
        if udev is None and not looks_like_joystick(joy.abs_codes_all, joy.key_codes_all):
            joy.close()
            continue
        key = (joy.name, joy.vid, joy.pid)
        if key in taken:
            taken.remove(key)  # one SDL2 device each
            joy.close()
            continue
        out[int(re.sub(r"\D", "", node.name))] = joy
    return out
