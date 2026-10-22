"""Press-to-bind controls (#279): the bindings the Controls page edits, free of Qt and SDL.

Wheels keep the SDK wheel driver's settings and formats (patches 0074, 0099, 0101):
`wheel_axes` ("steer=0,throttle=2,..."), `wheel_buttons` ("a=0,b=1,..."), `wheel_shifter_buttons`
("1=12,...,r=18" or "up=..,down=.."), plus the rotation, deadzone and invert switches. A field the
string leaves out keeps the driver's default, which this module ports (known Logitech wheels by
vendor:product) so the editor can show it.

Controllers use `controller_remap` (patch 0116): profiles separated by ';', each
"GUID:target=source,..." where GUID is SDL's joystick GUID (or '*' for every controller without a
profile of its own). A target is a pad control of the game's XInput state (a b x y lb rb back start
guide ls rs up down left right lt rt lx ly rx ry); a target left out keeps its own input. Sources:
a button (the targets' buttons and misc1 paddle1-4 touchpad), a trigger (lt rt), a stick axis
(lx ly rx ry, '-lx' inverted) or one direction of it (lx+ is right, ly+ is up), or none. '@0.3' after
an axis sets how far it must move to press a button (default 0.5). A button bound to a trigger
presses it fully. The SDK's parser (src/input/sdl/gamepad_remap.h) follows the same rules.
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

_C = "controller_remap"
CONTROLLER_ACTIONS = [
    Action("lx", "Steer", "stick", _C, "lx", "Driving"),
    Action("rt", "Accelerate", "trigger", _C, "rt", "Driving"),
    Action("lt", "Brake / reverse", "trigger", _C, "lt", "Driving"),
    Action("a", "Handbrake / select", "button", _C, "a", "Driving"),
    Action("b", "Back / shift up", "button", _C, "b", "Driving"),
    Action("x", "Enter event / shift down", "button", _C, "x", "Driving"),
    Action("y", "Rewind", "button", _C, "y", "Driving"),
    Action("rb", "Change camera", "button", _C, "rb", "Driving"),
    Action("lb", "Left bumper", "button", _C, "lb", "Driving"),
    Action("rs", "Look back (right stick press)", "button", _C, "rs", "Driving"),
    Action("back", "Open map", "button", _C, "back", "Menus and camera"),
    Action("start", "Pause", "button", _C, "start", "Menus and camera"),
    Action("ls", "Left stick press (friends)", "button", _C, "ls", "Menus and camera"),
    Action("ly", "Left stick up / down (menus)", "stick", _C, "ly", "Menus and camera"),
    Action("rx", "Look left / right", "stick", _C, "rx", "Menus and camera"),
    Action("ry", "Look up / down", "stick", _C, "ry", "Menus and camera"),
    Action("up", "D-pad up (radio)", "button", _C, "up", "Menus and camera"),
    Action("down", "D-pad down (radio)", "button", _C, "down", "Menus and camera"),
    Action("left", "D-pad left (radio)", "button", _C, "left", "Menus and camera"),
    Action("right", "D-pad right (radio)", "button", _C, "right", "Menus and camera"),
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


# --- controller remap --------------------------------------------------------------------------
PAD_BUTTONS = ("a", "b", "x", "y", "back", "guide", "start", "ls", "rs", "lb", "rb", "up", "down",
               "left", "right")
EXTRA_BUTTONS = ("misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad")
STICK_AXES = ("lx", "ly", "rx", "ry")
TRIGGERS = ("lt", "rt")
TARGETS = PAD_BUTTONS + TRIGGERS + STICK_AXES
DEFAULT_THRESHOLD = 0.5
_SOURCE = re.compile(r"^(-?)([a-z0-9]+)([+-]?)(?:@([0-9.]+))?$")


@dataclass(frozen=True)
class Source:
    """One input of the pad: a button, a whole axis (sign -1 inverts it) or one direction of a
    stick axis (half +1 or -1); threshold: how far an axis must move to press a button."""
    name: str
    half: int = 0
    invert: bool = False
    threshold: float = DEFAULT_THRESHOLD

    @property
    def is_button(self):
        return self.name in PAD_BUTTONS or self.name in EXTRA_BUTTONS

    @property
    def token(self):
        if self.name == "none":
            return "none"
        t = ("-" if self.invert else "") + self.name + {1: "+", -1: "-"}.get(self.half, "")
        if not self.is_button and abs(self.threshold - DEFAULT_THRESHOLD) > 1e-6:
            t += f"@{self.threshold:g}"
        return t

    def directions(self):
        """The physical input and the directions of it this source reads, for conflicts."""
        if self.is_button or self.name in TRIGGERS:
            return self.name, {1}
        return self.name, ({self.half} if self.half else {1, -1})


NONE = Source("none")


def parse_source(token):
    m = _SOURCE.match("".join((token or "").split()).lower())
    if not m:
        return None
    invert, name, half, threshold = m.groups()
    if name == "none" or name in PAD_BUTTONS or name in EXTRA_BUTTONS:
        if invert or half or threshold:
            return None
        return NONE if name == "none" else Source(name)
    if name not in STICK_AXES and name not in TRIGGERS:
        return None
    if name in TRIGGERS and (invert or half):
        return None
    if invert and half:
        return None
    t = DEFAULT_THRESHOLD
    if threshold:
        try:
            t = min(0.95, max(0.05, float(threshold)))
        except ValueError:
            return None
    return Source(name, {"+": 1, "-": -1}.get(half, 0), bool(invert), t)


def accepts(target, source):
    """Whether the SDK applies `source` to `target` (unsupported pairs are ignored there)."""
    if source is NONE or source.name == "none":
        return True
    if target in STICK_AXES:
        return source.name in STICK_AXES and not source.half
    if target in TRIGGERS:
        return source.is_button or source.name in TRIGGERS or (source.name in STICK_AXES and not source.invert)
    return target in PAD_BUTTONS and (source.is_button or not source.invert)


def parse_rules(text):
    """"rt=a,a=none" to {"rt": Source("a"), "a": NONE}; unknown or unsupported pairs are dropped."""
    rules = {}
    for item in (text or "").split(","):
        if "=" not in item:
            continue
        target, token = ("".join(p.split()).lower() for p in item.split("=", 1))
        source = parse_source(token)
        if target in TARGETS and source is not None and accepts(target, source):
            rules[target] = source
    return rules


def format_rules(rules):
    return ",".join(f"{t}={rules[t].token}" for t in TARGETS if t in rules)


def parse_remap(text):
    """{profile key: rules}; a profile key is a GUID (lower case) or '*'."""
    profiles = {}
    for part in (text or "").split(";"):
        if ":" not in part:
            continue
        key, rules = part.split(":", 1)
        key = key.strip().lower()
        if key == "*" or re.fullmatch(r"[0-9a-f]{32}", key):
            profiles[key] = parse_rules(rules)
    return profiles


def format_remap(profiles):
    """'*' comes last. A profile without rules is left out unless it keeps a controller off '*'."""
    keys = sorted(k for k in profiles if k != "*") + (["*"] if "*" in profiles else [])
    keep = [k for k in keys if profiles[k] or (k != "*" and profiles.get("*"))]
    return ";".join(f"{k}:{format_rules(profiles[k])}" for k in keep)


def guid_model(guid):
    """(vendor, product) from an SDL GUID that carries them (bytes 4-5 and 8-9, 6-7 and 10-11 zero),
    else None (a GUID built from the name)."""
    try:
        b = bytes.fromhex(guid)
    except (TypeError, ValueError):
        return None
    if len(b) != 16 or b[6:8] != b"\0\0" or b[10:12] != b"\0\0":
        return None
    vid, pid = int.from_bytes(b[4:6], "little"), int.from_bytes(b[8:10], "little")
    return (vid, pid) if vid else None


def profile_key(profiles, guid):
    """The profile a controller uses: its own GUID, else one of the same model (vendor and product,
    whichever connection or driver SDL reports), else '*'. None when there is none."""
    guid = (guid or "").lower()
    if guid in profiles:
        return guid
    model = guid_model(guid)
    if model:
        for key in profiles:
            if key != "*" and guid_model(key) == model:
                return key
    return "*" if "*" in profiles else None


def controller_rules(text, guid):
    profiles = parse_remap(text)
    key = profile_key(profiles, guid)
    return dict(profiles[key]) if key else {}


def controller_binding(rules, action):
    """The source that drives `action` (its own input when the profile does not bind it)."""
    if action.field in rules:
        return rules[action.field], False
    return Source(action.field), True


def set_controller_binding(text, guid, action, source):
    """The new controller_remap string with `action` bound to `source` in this controller's own
    profile (made from the profile it used so far); source None clears, "default" resets."""
    profiles = parse_remap(text)
    key = (guid or "*").lower()
    if key not in profiles:
        used = profile_key(profiles, guid)
        profiles[key] = dict(profiles[used]) if used else {}
    rules = profiles[key]
    if source == "default":
        rules.pop(action.field, None)
    else:
        rules[action.field] = NONE if source is None else source
    return format_remap(profiles)


AXIS_NAMES = {"lx": "Left stick X", "ly": "Left stick Y", "rx": "Right stick X", "ry": "Right stick Y",
              "lt": "LT", "rt": "RT"}
HALF_NAMES = {("lx", 1): "Left stick right", ("lx", -1): "Left stick left",
              ("ly", 1): "Left stick up", ("ly", -1): "Left stick down",
              ("rx", 1): "Right stick right", ("rx", -1): "Right stick left",
              ("ry", 1): "Right stick up", ("ry", -1): "Right stick down"}
BUTTON_NAMES = {"a": "A", "b": "B", "x": "X", "y": "Y", "back": "Back", "guide": "Guide",
                "start": "Start", "ls": "LS", "rs": "RS", "lb": "LB", "rb": "RB", "up": "D-pad up",
                "down": "D-pad down", "left": "D-pad left", "right": "D-pad right", "misc1": "Share",
                "paddle1": "Paddle 1", "paddle2": "Paddle 2", "paddle3": "Paddle 3",
                "paddle4": "Paddle 4", "touchpad": "Touchpad"}


def source_text(source, names=None):
    """A player-facing name; `names` replaces button names (PlayStation prompts)."""
    names = {**BUTTON_NAMES, **(names or {})}
    if source.name == "none":
        return "Not bound"
    if source.is_button:
        return names.get(source.name, source.name)
    if source.half:
        text = HALF_NAMES[(source.name, source.half)]
    else:
        text = names.get(source.name, AXIS_NAMES[source.name])
        if source.invert:
            text += " (inverted)"
    if abs(source.threshold - DEFAULT_THRESHOLD) > 1e-6:
        text += f" past {round(source.threshold * 100)}%"
    return text


# Without a remap the SDK's driver also presses Y, B, X and A with the Elite paddles.
DEFAULT_EXTRA = {"y": "paddle1", "b": "paddle2", "x": "paddle3", "a": "paddle4"}


def controller_conflicts(rules, actions=CONTROLLER_ACTIONS, names=None):
    """[(input text, [row labels])] for physical inputs more than one row reads (a whole stick axis
    overlaps either of its directions)."""
    used = []
    for action in actions:
        source, default = controller_binding(rules, action)
        if source.name == "none":
            continue
        used.append((action, source))
        if default and action.field in DEFAULT_EXTRA:
            used.append((action, Source(DEFAULT_EXTRA[action.field])))
    out = []
    seen = set()
    for i, (action, source) in enumerate(used):
        if i in seen:
            continue
        name, dirs = source.directions()
        group = [action.label]
        for j in range(i + 1, len(used)):
            other_name, other_dirs = used[j][1].directions()
            if other_name == name and dirs & other_dirs:
                seen.add(j)
                if used[j][0].label not in group:
                    group.append(used[j][0].label)
        if len(group) > 1:
            out.append((source_text(Source(name), names) if source.is_button or name in TRIGGERS
                        else AXIS_NAMES[name], group))
    return out


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
    kind: str           # "axis" or "button" (wheel); "source" (controller)
    number: int = -1    # axis or button number
    direction: int = 0  # axis: +1 moved up from its rest, -1 down
    rest: int = 0       # axis: where it rested when binding started
    source: Source | None = None

    def describe(self):
        if self.kind == "button":
            return f"Button {self.number}"
        if self.kind == "axis":
            return (f"Axis {self.number}, {'up' if self.direction > 0 else 'down'} from "
                    f"{rest_text(self.rest)}")
        return source_text(self.source)


def rest_text(raw):
    v = norm(raw)
    return "the maximum" if v > .8 else "the minimum" if v < -.8 else "the centre" \
        if abs(v) < .2 else f"{round((v + 1) * 50)}%"


class Detector:
    """Waits for the first input that changes from where it was when binding started. kind: a
    wheel row's kind ("axis", "button"; "steer" takes a smaller turn) or, with controller, a
    controller target's ("button", "trigger", "stick"; the mapped gamepad inputs).
    Inputs held at the start count only after they were released."""
    def __init__(self, baseline, kind, controller=False, threshold=None):
        self.base = baseline
        self.kind = kind
        self.controller = controller
        self.threshold = threshold if threshold is not None else (
            0.15 if kind == "steer" else 0.5)
        self.released = set()

    def feed(self, snap):
        if self.controller:
            return self._pad(snap)
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

    def _pad(self, snap):
        base_axes = self.base.pad_axes or (0.0,) * 6
        names = ("lx", "ly", "rx", "ry", "lt", "rt")
        if self.kind != "stick":
            for name in PAD_BUTTONS + EXTRA_BUTTONS:
                down = name in snap.pad_buttons
                held = name in self.base.pad_buttons
                if held and not down:
                    self.released.add(name)
                if down and (not held or name in self.released) and name != "guide":
                    return Detection("source", source=Source(name))
        for i, name in enumerate(names):
            if i >= len(snap.pad_axes):
                break
            delta = snap.pad_axes[i] - base_axes[i]
            if abs(delta) < self.threshold:
                continue
            sign = 1 if delta > 0 else -1
            if name in TRIGGERS:
                if self.kind != "stick" and sign > 0:
                    return Detection("source", source=Source(name))
                continue
            if self.kind == "stick":
                # The row asks for right or up: the other way binds the axis inverted.
                return Detection("source", source=Source(name, invert=sign < 0))
            return Detection("source", source=Source(name, half=sign))
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


# --- calibration -------------------------------------------------------------------------------
def steering_value(raw, rotation, lock, deadzone, invert):
    """The steering the driver sends (-1 to 1) for an axis value: scaled by rotation / lock,
    inverted, then the deadzone (the steps of ComputeStateLocked)."""
    s = norm(raw) * (rotation / max(1, lock)) * (-1.0 if invert else 1.0)
    a = abs(s)
    if a <= deadzone or deadzone >= 1:
        return 0.0
    out = min(1.0, (a - deadzone) / (1.0 - deadzone))
    return out if s > 0 else -out


def rotation_from_turn(raw, centre_raw=0, degrees=90):
    """The wheel's rotation (lock to lock, degrees) from the axis read with the wheel turned
    `degrees` from the centre: the axis spans the whole rotation, so one side of it is half.
    Rounded to the setting's 10 degree steps; None when the wheel was not turned."""
    moved = abs(norm(raw) - norm(centre_raw))
    if moved < 0.02:
        return None
    return int(max(90, min(2520, round(2 * degrees / moved / 10) * 10)))


def steering_calibration(left_raw, right_raw):
    """(invert, travel) from the axis at full left lock and at full right lock."""
    left, right = norm(left_raw), norm(right_raw)
    return left > right, min(1.0, abs(right - left) / 2.0)


def busiest_axis(baseline, snap, exclude=()):
    """(axis, distance) of the axis furthest from where it was in `baseline`."""
    best, best_delta = None, 0.0
    for n, raw in enumerate(snap.axes):
        if n in exclude or n >= len(baseline.axes):
            continue
        delta = abs(norm(raw) - norm(baseline.axes[n]))
        if delta > best_delta:
            best, best_delta = n, delta
    return best, best_delta


class Calibration:
    """Wheel calibration, step by step: centre (wheel centred, pedals released), full left and full
    right lock (the steering axis, its direction and travel), a quarter turn (the rotation), then
    each pedal pressed fully (its axis, travel and invert switch). Any step but the first can be
    skipped, which keeps what the settings have for it. `known`: a wheel with a built-in profile
    (its pedals are taken to rest at the maximum)."""
    STEPS = ("centre", "left", "right", "quarter", "throttle", "brake", "clutch", "finish")
    PROMPTS = {
        "centre": "Centre the wheel and release every pedal, then press Next.",
        "left": "Turn the wheel all the way to the left, to its lock, and hold it there.",
        "right": "Now turn it all the way to the right, to its lock, and hold it there.",
        "quarter": "Turn the wheel a quarter turn to the right from the centre: the top of the "
                   "rim points right (90°). Hold it there.",
        "throttle": "Press the throttle pedal all the way down and release it.",
        "brake": "Press the brake pedal all the way down and release it. "
                 "One pedal axis for both (combined pedals)? Press the brake side.",
        "clutch": "Press the clutch pedal all the way down and release it (Skip without one).",
        "finish": "Check the steering deadzone, then Apply.",
    }
    MIN_MOVE = {"left": 0.3, "right": 0.3, "quarter": 0.02, "throttle": 0.3, "brake": 0.3, "clutch": 0.3}

    def __init__(self, known=False):
        self.known = known
        self.index = 0
        self.base = None
        self.steer = None   # axis
        self.left = self.right = None  # raw values at the locks
        self.quarter = None
        self.pedals = {}    # name -> (axis, rest, full)
        self.peak = None    # (axis, raw furthest from the centre snapshot) in a pedal step
        self.far = {}       # axis -> (distance, raw) furthest from the centre in this step
        self.last = None

    @property
    def step(self):
        return self.STEPS[self.index]

    @property
    def done(self):
        return self.step == "finish"

    def exclude(self):
        used = {self.steer} | {p[0] for n, p in self.pedals.items()}
        if self.step == "brake" and "throttle" in self.pedals:
            used.discard(self.pedals["throttle"][0])  # combined pedals share the axis
        return used - {None}

    def feed(self, snap):
        """Track the step's input; returns a line describing what is seen."""
        self.last = snap
        step = self.step
        if self.base is None or step in ("centre", "finish"):
            return ""
        if step in ("left", "right", "quarter"):
            axis = self.steer
            if axis is None:
                axis, _ = busiest_axis(self.base, snap, {p[0] for p in self.pedals.values()})
            if axis is None or axis >= len(snap.axes):
                return ""
            return f"Steering: axis {axis} at {round(norm(snap.axes[axis]) * 100)}%"
        excluded = self.exclude()
        for n, raw in enumerate(snap.axes):
            if n in excluded or n >= len(self.base.axes):
                continue
            moved = norm(raw) - norm(self.base.axes[n])
            key = (n, 1 if moved > 0 else -1)
            if abs(moved) > self.far.get(key, (0.0, 0))[0]:
                self.far[key] = (abs(moved), raw)
        t = self.pedals.get("throttle")
        # Only the other side of the throttle's axis can be a brake (combined pedals).
        taken = (t[0], 1 if norm(t[2]) > norm(t[1]) else -1) if step == "brake" and t else None
        best = max((k for k in self.far if k != taken), key=lambda k: self.far[k][0], default=None)
        if best is not None and self.far[best][0] >= self.MIN_MOVE[step] / 2:
            self.peak = (best[0], self.far[best][1])
        if self.peak is None:
            return "Waiting for a pedal…"
        axis, raw = self.peak
        travel = abs(norm(raw) - norm(self.base.axes[axis])) / 2
        return f"{step.capitalize()}: axis {axis}, pressed {round(travel * 100)}% of its travel"

    def can_advance(self):
        step = self.step
        if step == "centre":
            return self.last is not None
        if step == "finish":
            return False
        if step in ("left", "right", "quarter"):
            if self.last is None or self.base is None:
                return False
            axis = self.steer
            if axis is None:
                axis, delta = busiest_axis(self.base, self.last, {p[0] for p in self.pedals.values()})
            else:
                delta = abs(norm(self.last.axes[axis]) - norm(self.base.axes[axis]))
            return axis is not None and delta >= self.MIN_MOVE[step]
        return self.peak is not None and \
            abs(norm(self.peak[1]) - norm(self.base.axes[self.peak[0]])) >= self.MIN_MOVE[step]

    def next(self):
        """Record the step from what was fed, then go on."""
        if not self.can_advance():
            return False
        step, snap = self.step, self.last
        if step == "centre":
            self.base = snap
        elif step in ("left", "right", "quarter"):
            if self.steer is None:
                self.steer, _ = busiest_axis(self.base, snap, {p[0] for p in self.pedals.values()})
            setattr(self, step, snap.axes[self.steer])
        else:
            axis, raw = self.peak
            self.pedals[step] = (axis, self.base.axes[axis], raw)
        self.peak, self.far = None, {}
        self.index += 1
        return True

    def skip(self):
        if self.step in ("centre", "finish"):
            return False
        self.peak, self.far = None, {}
        self.index += 1
        return True

    def back(self):
        if self.index == 0:
            return
        self.index -= 1
        self.peak, self.far = None, {}
        step = self.step
        if step in ("left", "right", "quarter"):
            setattr(self, step, None)
            if step == "left":
                self.steer = None
        elif step in self.pedals:
            del self.pedals[step]
        elif step == "centre":
            self.base = None

    def combined(self):
        """Throttle and brake were found on one axis, in opposite directions."""
        t, br = self.pedals.get("throttle"), self.pedals.get("brake")
        return bool(t and br and t[0] == br[0] and (norm(t[2]) > norm(t[1])) != (norm(br[2]) > norm(br[1])))

    def rotation(self):
        if self.quarter is None or self.base is None or self.steer is None:
            return None
        return rotation_from_turn(self.quarter, self.base.axes[self.steer])

    def results(self, settings):
        """(lines, warnings) to show before Apply."""
        lines, warnings = [], []
        if self.steer is not None and self.left is not None and self.right is not None:
            invert, travel = steering_calibration(self.left, self.right)
            lines.append(f"Steering: axis {self.steer}{', inverted' if invert else ''}")
            if travel < 0.9:
                warnings.append(f"The wheel reaches only {int(travel * 100)}% of its axis at full "
                                "lock: check the rotation set in the wheel's own software.")
        rotation = self.rotation()
        if rotation:
            lines.append(f"Rotation: {rotation}° lock to lock")
        for name in ("throttle", "brake", "clutch"):
            if name not in self.pedals:
                continue
            axis, rest, full = self.pedals[name]
            if self.combined() and name == "brake":
                lines.append(f"Brake: the other side of axis {axis} (combined pedals)")
                continue
            invert, travel = pedal_calibration(rest, full, self.known)
            lines.append(f"{name.capitalize()}: axis {axis}, rests at {rest_text(rest)}"
                         f"{', inverted' if invert else ''}")
            if travel < 0.9 and not (self.combined() and name == "throttle"):
                warnings.append(f"The {name} reaches only {int(travel * 100)}% of its axis, so a "
                                f"full press gives {int(travel * 100)}% {name}: check the pedal's "
                                "range in the wheel's own software.")
        if not self.known and self.pedals:
            warnings.append("Keep the pedals released while the game starts: it reads where they "
                            "rest then.")
        return lines, warnings

    def changes(self, settings, deadzone=None):
        """The settings Apply writes. Steps that were skipped leave theirs alone."""
        out = {}
        axes = parse_kv(settings.get("wheel_axes", ""))
        if self.steer is not None and self.left is not None and self.right is not None:
            axes["steer"] = self.steer
            out["wheel_invert_steering"], _ = steering_calibration(self.left, self.right)
        rotation = self.rotation()
        if rotation:
            out["wheel_rotation"] = rotation
            if int(settings.get("wheel_lock", 900)) > rotation:
                out["wheel_lock"] = rotation  # full lock must still give full steering
        if self.combined():
            axis, rest, full = self.pedals["throttle"]
            axes["throttle"] = axis
            axes.pop("brake", None)
            out["wheel_pedals"] = "combined"
            # Combined: the driver takes the axis below its centre as throttle unless inverted.
            out["wheel_invert_throttle"] = norm(full) > norm(rest)
        else:
            for name in ("throttle", "brake"):
                if name in self.pedals:
                    axis, rest, full = self.pedals[name]
                    axes[name] = axis
                    out[f"wheel_invert_{name}"], _ = pedal_calibration(rest, full, self.known)
            if "throttle" in self.pedals and "brake" in self.pedals and \
                    settings.get("wheel_pedals") == "combined":
                out["wheel_pedals"] = "auto"
        if "clutch" in self.pedals:
            axis, rest, full = self.pedals["clutch"]
            axes["clutch"] = axis
            out["wheel_invert_clutch"], _ = pedal_calibration(rest, full, self.known)
        if axes != parse_kv(settings.get("wheel_axes", "")):
            out["wheel_axes"] = format_kv(axes, AXIS_ORDER)
        if deadzone is not None:
            out["wheel_deadzone"] = round(float(deadzone), 2)
        return out
