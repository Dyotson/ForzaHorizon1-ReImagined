"""Press-to-bind controls (#279): the bindings the Controls page edits, free of Qt and SDL.

Wheels keep the SDK wheel driver's settings and formats (patches 0074, 0099, 0101):
`wheel_axes` ("steer=0,throttle=2,..."), `wheel_buttons` ("a=0,b=1,..."), `wheel_shifter_buttons`
("1=12,...,r=18" or "up=..,down=.."), plus the rotation, deadzone and invert switches. A field the
string leaves out keeps the driver's default, which this module ports (known Logitech wheels by
vendor:product) so the editor can show it.
"""
from dataclasses import dataclass
import re

# --- key=value strings -------------------------------------------------------------------------
_ATOI = re.compile(r"\s*([+-]?\d+)")


def parse_kv(text):
    """"a=0,b=1" to {"a": 0, "b": 1}, as the SDK's ParseKv: keys lower case without spaces, the
    value read like atoi (0 when it is not a number); items without '=' are skipped."""
    out = {}
    for item in str(text or "").split(","):
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        key = "".join(key.split()).lower()
        m = _ATOI.match(value)
        out[key] = int(m.group(1)) if m else 0
    return out


def format_kv(values, order=()):
    """{"steer": 0, "throttle": 2} to "steer=0,throttle=2": known keys in `order`, others after."""
    keys = [k for k in order if k in values] + sorted(k for k in values if k not in order)
    return ",".join(f"{k}={int(values[k])}" for k in keys)


# --- actions -----------------------------------------------------------------------------------
@dataclass(frozen=True)
class Action:
    key: str      # row id
    label: str
    kind: str     # wheel: axis, button; controller: button, trigger, stick
    setting: str  # the forza.toml key it is stored in
    field: str    # its key inside that setting's string
    section: str


# The game takes a wheel for the Xbox 360 wheel and applies its wheel layout 0 (gamedb.slt
# WheelLayouts): the bumpers shift, A is the clutch button, B the handbrake, X Activate, Y Rewind,
# Back the camera.
_W = "wheel_buttons"
WHEEL_ACTIONS = [
    Action("steer", "Steering", "axis", "wheel_axes", "steer", "Wheel and pedals"),
    Action("throttle", "Throttle", "axis", "wheel_axes", "throttle", "Wheel and pedals"),
    Action("brake", "Brake", "axis", "wheel_axes", "brake", "Wheel and pedals"),
    Action("clutch", "Clutch pedal", "axis", "wheel_axes", "clutch", "Wheel and pedals"),
    Action("rb", "Shift up (RB, right paddle)", "button", _W, "rb", "Buttons"),
    Action("lb", "Shift down (LB, left paddle)", "button", _W, "lb", "Buttons"),
    Action("a", "Clutch button (A)", "button", _W, "a", "Buttons"),
    Action("b", "Handbrake (B)", "button", _W, "b", "Buttons"),
    Action("x", "Activate / enter event (X)", "button", _W, "x", "Buttons"),
    Action("y", "Rewind (Y)", "button", _W, "y", "Buttons"),
    Action("back", "Change camera (Back)", "button", _W, "back", "Buttons"),
    Action("start", "Pause (Start)", "button", _W, "start", "Buttons"),
    Action("rs", "Right stick press (RS)", "button", _W, "rs", "Buttons"),
    Action("ls", "Left stick press (LS)", "button", _W, "ls", "Buttons"),
    Action("up", "D-pad up", "button", _W, "up", "Buttons"),
    Action("down", "D-pad down", "button", _W, "down", "Buttons"),
    Action("left", "D-pad left", "button", _W, "left", "Buttons"),
    Action("right", "D-pad right", "button", _W, "right", "Buttons"),
    *[Action(f"gear{g}", f"Gear {g}", "button", "wheel_shifter_buttons", str(g), "Shifter")
      for g in range(1, 8)],
    Action("gearr", "Reverse gear", "button", "wheel_shifter_buttons", "r", "Shifter"),
    Action("seq_up", "Sequential shift up", "button", "wheel_shifter_buttons", "up", "Shifter"),
    Action("seq_down", "Sequential shift down", "button", "wheel_shifter_buttons", "down", "Shifter"),
]

AXIS_ORDER = ("steer", "throttle", "brake", "clutch")
BUTTON_ORDER = ("a", "b", "x", "y", "lb", "rb", "back", "start", "ls", "rs", "up", "down", "left", "right")
SHIFTER_ORDER = ("1", "2", "3", "4", "5", "6", "7", "r", "up", "down")
# An axis number past every wheel's axes: the driver reads no axis for it. Negative numbers mean
# "the default" to the driver (the profile or detection), so they cannot unbind an axis.
NO_AXIS = 99

# --- wheel defaults, as the driver has them --------------------------------------------------
_LOGITECH = 0x046D
KNOWN_WHEELS = {0xC24F: "Logitech G29", 0xC262: "Logitech G920", 0xC266: "Logitech G923",
                0xC26E: "Logitech G923 (Xbox)", 0xC29A: "Logitech Driving Force GT",
                0xC29B: "Logitech G27", 0xC299: "Logitech G25"}
_PLAYSTATION = {0xC24F, 0xC266, 0xC267}
_XBOX = {0xC262, 0xC26E, 0xC26D}
WHEEL_WORDS = ("wheel", "racing", "driving force", "g25", "g27", "g29", "g920", "g923", "t300",
               "t150", "t248", "t-gt", "tx racing", "fanatec", "moza", "simagic", "csl", "podium",
               "ts-xw")


def known_wheel(vid, pid):
    return KNOWN_WHEELS.get(pid) if vid == _LOGITECH else None


def looks_like_wheel(name, vid=0, sdl_wheel=False):
    """The driver's wheel_mode = auto test (IsWheelDevice): Microsoft pads never are."""
    if vid == 0x045E:
        return False
    return bool(sdl_wheel) or any(w in (name or "").lower() for w in WHEEL_WORDS)


def default_axes(vid, pid, windows):
    """A known wheel's fixed axes; {} for others (the driver detects them when it opens the wheel)."""
    if not known_wheel(vid, pid):
        return {}
    if windows:
        return {"steer": 0, "throttle": 1, "brake": 2, "clutch": 3}
    return {"steer": 0, "clutch": 1, "throttle": 2, "brake": 3}


def default_buttons(vid, pid):
    m = {"a": 0, "b": 1, "x": 2, "y": 3, "rb": 4, "lb": 5, "back": 8, "start": 9, "rs": 10,
         "ls": 11, "up": -1, "down": -1, "left": -1, "right": -1}
    if vid == _LOGITECH and pid in _PLAYSTATION:
        m.update(b=2, x=1)
    elif vid == _LOGITECH and pid in _XBOX:
        m.update(start=6, back=7, rs=8, ls=9)
    return m


def default_shifter(vid, pid):
    m = {k: -1 for k in SHIFTER_ORDER}
    if vid == _LOGITECH and (pid in _PLAYSTATION or pid in _XBOX):
        m.update({str(g): 11 + g for g in range(1, 7)})
        m["r"] = 18 if pid in _PLAYSTATION else 11
    return m


@dataclass(frozen=True)
class WheelInfo:
    vid: int = 0
    pid: int = 0
    num_axes: int = 0
    num_buttons: int = 0
    num_hats: int = 0
    windows: bool = False


def wheel_binding(action, settings, wheel):
    """(number or None, is_default) for a wheel row. None: no input (or one detected at start)."""
    own = parse_kv(settings.get(action.setting, "")).get(action.field)
    if action.setting == "wheel_axes":
        if own is not None and own >= 0:
            return (None if own >= NO_AXIS else own), False
        return default_axes(wheel.vid, wheel.pid, wheel.windows).get(action.field), True
    if action.setting == "wheel_buttons":
        if own is not None:
            return (own if own >= 0 else None), False
        n = default_buttons(wheel.vid, wheel.pid)[action.field]
        return (n if n >= 0 else None), True
    if own is not None:
        return (own if own >= 0 else None), False
    n = default_shifter(wheel.vid, wheel.pid)[action.field]
    return (n if n >= 0 else None), True


def wheel_binding_text(action, settings, wheel):
    n, default = wheel_binding(action, settings, wheel)
    if action.kind == "axis":
        if n is None:
            text = "Detected at start" if default else "Not bound"
        else:
            text = f"Axis {n}"
    elif n is None:
        text = "Hat (d-pad)" if action.field in ("up", "down", "left", "right") and default \
            and wheel.num_hats else "Not bound"
    else:
        text = f"Button {n}"
    return text + (" (default)" if default and n is not None else "")


def set_wheel_binding(settings, action, number):
    """The settings changes ({key: new string}) that bind `action` to `number`; None clears it and
    "default" removes the field so the driver's default applies again."""
    values = parse_kv(settings.get(action.setting, ""))
    if number == "default":
        values.pop(action.field, None)
    elif number is None:
        values[action.field] = NO_AXIS if action.kind == "axis" else -1
    else:
        values[action.field] = int(number)
    order = {"wheel_axes": AXIS_ORDER, "wheel_buttons": BUTTON_ORDER}.get(action.setting, SHIFTER_ORDER)
    return {action.setting: format_kv(values, order)}


def wheel_conflicts(settings, wheel):
    """[(input text, [row labels])] for every axis or button that more than one row uses. Shifter
    rows count only for the shifter mode that reads them."""
    shifter = settings.get("wheel_shifter", "off")
    used = {}
    for action in WHEEL_ACTIONS:
        if action.setting == "wheel_shifter_buttons":
            sequential = action.field in ("up", "down")
            if shifter == "off" or (shifter == "h") == sequential:
                continue
        n, _ = wheel_binding(action, settings, wheel)
        if n is None:
            continue
        used.setdefault((action.kind, n), []).append(action.label)
    return [(f"{'Axis' if kind == 'axis' else 'Button'} {n}", labels)
            for (kind, n), labels in sorted(used.items()) if len(labels) > 1]


# --- detection ---------------------------------------------------------------------------------
def norm(raw):
    """An SDL axis value as the wheel driver reads it (Norm): -1 to 1."""
    return raw / 32768.0 if raw < 0 else raw / 32767.0


@dataclass(frozen=True)
class Snapshot:
    """A joystick's raw state: axes -32768..32767, buttons, hats (SDL hat bits). For a controller
    with a gamepad mapping, `pad` has the mapped inputs too: buttons by name and the axes
    lx ly rx ry (up and right positive) and lt rt (0 to 1)."""
    axes: tuple = ()
    buttons: tuple = ()
    hats: tuple = ()
    pad_buttons: frozenset = frozenset()
    pad_axes: tuple = ()  # (lx, ly, rx, ry, lt, rt)


@dataclass(frozen=True)
class Detection:
    kind: str           # "axis" or "button"
    number: int = -1    # axis or button number
    direction: int = 0  # axis: +1 moved up from its rest, -1 down
    rest: int = 0       # axis: where it rested when binding started

    def describe(self):
        if self.kind == "button":
            return f"Button {self.number}"
        return (f"Axis {self.number}, {'up' if self.direction > 0 else 'down'} from "
                f"{rest_text(self.rest)}")


def rest_text(raw):
    v = norm(raw)
    return "the maximum" if v > .8 else "the minimum" if v < -.8 else "the centre" \
        if abs(v) < .2 else f"{round((v + 1) * 50)}%"


class Detector:
    """Waits for the first input that changes from where it was when binding started. kind: a
    wheel row's kind ("axis", "button"; "steer" takes a smaller turn).
    Inputs held at the start count only after they were released."""
    def __init__(self, baseline, kind, threshold=None):
        self.base = baseline
        self.kind = kind
        self.threshold = threshold if threshold is not None else (
            0.15 if kind == "steer" else 0.5)
        self.released = set()

    def feed(self, snap):
        if self.kind in ("axis", "steer"):
            for n, raw in enumerate(snap.axes):
                if n >= len(self.base.axes):
                    break
                delta = norm(raw) - norm(self.base.axes[n])
                if abs(delta) >= self.threshold:
                    return Detection("axis", n, 1 if delta > 0 else -1, self.base.axes[n])
            return None
        for n, down in enumerate(snap.buttons):
            held = n < len(self.base.buttons) and self.base.buttons[n]
            if held and not down:
                self.released.add(n)
            if down and (not held or n in self.released):
                return Detection("button", n)
        return None


# --- pedals -------------------------------------------------------------------------------
def pedal_value(raw, open_raw, invert, known):
    """How far a pedal is pressed (0 to 1), as the wheel driver computes it (`pressed`) once the
    pedal was seen at rest: open_raw is where the axis sat when the game opened the wheel."""
    v = norm(raw)
    r = 1.0 if known else norm(open_raw)
    high_rest = r > 0.0
    if invert:
        high_rest = not high_rest
    rest = (1.0 if high_rest else -1.0) if (invert or known) else r
    if high_rest:
        p = (rest - v) / (rest + 1.0) if rest + 1.0 else 0.0
    else:
        p = (v - rest) / (1.0 - rest) if 1.0 - rest else 0.0
    d = 0.02
    return min(1.0, max(0.0, (p - d) / (1.0 - d)))


def pedal_calibration(rest_raw, full_raw, known):
    """(invert, travel) for a pedal seen at rest and fully pressed. invert is the switch under which
    the driver reads the rest as released and the full press as pressed; travel is the share of
    the axis the pedal covers (a pedal that stops short never reaches a full press)."""
    def error(invert):
        return pedal_value(rest_raw, rest_raw, invert, known) + \
            (1.0 - pedal_value(full_raw, rest_raw, invert, known))
    invert = error(True) + 1e-6 < error(False)
    travel = abs(norm(full_raw) - norm(rest_raw)) / 2.0
    return invert, min(1.0, travel)
