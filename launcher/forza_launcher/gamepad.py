"""SDL controller input, independent of Qt. Only the launcher UI initializes SDL."""
import ctypes
import os
import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class State:
    device: int
    buttons: frozenset[str] = frozenset()
    x: int = 0
    y: int = 0


class Actions:
    """Edge-trigger buttons; repeat directions, and require neutral after reconnect/resume."""
    def __init__(self):
        self.reset()

    def reset(self):
        self.device = None
        self.armed = False
        self.held = set()
        self.direction = None
        self.repeat_at = 0

    def update(self, state, now, enabled=True):
        if not enabled or state is None:
            self.reset()
            return []
        if state.device != self.device:
            self.reset()
            self.device = state.device
        held = set(state.buttons)
        # Use the dominant axis and a generous dead zone to reject resting-stick drift.
        if max(abs(state.x), abs(state.y)) > 16000:
            if abs(state.x) > abs(state.y):
                held.add("right" if state.x > 0 else "left")
            else:
                held.add("down" if state.y > 0 else "up")
        if not self.armed:
            self.armed = not held
            return []
        directions = held & {"up", "down", "left", "right"}
        direction = next(iter(directions)) if len(directions) == 1 else None
        actions = [b for b in ("back", "previous", "next", "accept")
                   if b in held and b not in self.held]
        if direction != self.direction:
            if direction:
                actions.append(direction)
            self.repeat_at = now + .4
        elif direction and now >= self.repeat_at:
            actions.append(direction)
            self.repeat_at = now + .12
        self.direction = direction
        self.held = held
        # Never activate something in the same sample that changes the page or backs out.
        return actions[:1]


class SDLController:
    """One mapped controller at a time, with hotplug and no video/audio initialization.

    SDL's mapping database handles Xbox, PlayStation and Steam/Deck controllers.
    Background events are needed because the window belongs to Qt; the UI gates input.
    """
    def __init__(self):
        import sdl2
        self.sdl = sdl2
        self.controller = None
        # Qt owns the event loop. SDL turns SIGTERM into an SDL_QUIT event that
        # this controller-only backend discards, preventing normal termination.
        sdl2.SDL_SetHint(sdl2.SDL_HINT_NO_SIGNAL_HANDLERS, b"1")
        sdl2.SDL_SetHint(sdl2.SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, b"1")
        if sdl2.SDL_InitSubSystem(sdl2.SDL_INIT_GAMECONTROLLER) != 0:
            raise RuntimeError(sdl2.SDL_GetError().decode("utf-8", "replace"))
        self.initialized = True

    def read(self):
        s = self.sdl
        s.SDL_PumpEvents()
        s.SDL_GameControllerUpdate()
        # We own SDL in this process; discard events after reading the current state.
        s.SDL_FlushEvents(s.SDL_FIRSTEVENT, s.SDL_LASTEVENT)
        if self.controller and not s.SDL_GameControllerGetAttached(self.controller):
            s.SDL_GameControllerClose(self.controller)
            self.controller = None
        if not self.controller:
            for i in range(s.SDL_NumJoysticks()):
                if s.SDL_IsGameController(i):
                    self.controller = s.SDL_GameControllerOpen(i)
                    if self.controller:
                        break
        if not self.controller:
            return None
        buttons = {
            "accept": s.SDL_CONTROLLER_BUTTON_A, "back": s.SDL_CONTROLLER_BUTTON_B,
            "previous": s.SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
            "next": s.SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            "up": s.SDL_CONTROLLER_BUTTON_DPAD_UP, "down": s.SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            "left": s.SDL_CONTROLLER_BUTTON_DPAD_LEFT, "right": s.SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        }
        return State(
            s.SDL_JoystickInstanceID(s.SDL_GameControllerGetJoystick(self.controller)),
            frozenset(name for name, button in buttons.items()
                      if s.SDL_GameControllerGetButton(self.controller, button)),
            s.SDL_GameControllerGetAxis(self.controller, s.SDL_CONTROLLER_AXIS_LEFTX),
            s.SDL_GameControllerGetAxis(self.controller, s.SDL_CONTROLLER_AXIS_LEFTY),
        )

    def close(self):
        if self.controller:
            self.sdl.SDL_GameControllerClose(self.controller)
            self.controller = None
        if self.initialized:
            self.sdl.SDL_QuitSubSystem(self.sdl.SDL_INIT_GAMECONTROLLER)
            self.initialized = False


@dataclass(frozen=True)
class DeviceInfo:
    instance: int
    name: str
    guid: str
    vid: int
    pid: int
    gamepad: bool  # SDL has a gamepad mapping for it
    sdl_wheel: bool  # SDL says it is a wheel


class Joysticks:
    """Every SDL joystick, raw, for the Controls page's bindings editor: one opened at a time.

    Shares the process's SDL with SDLController (each holds its own reference to the joystick
    subsystem). The raw axis and button numbers are the ones the game's wheel driver reads.
    """
    PAD_BUTTONS = ("a", "b", "x", "y", "back", "guide", "start", "ls", "rs", "lb", "rb", "up",
                   "down", "left", "right", "misc1", "paddle1", "paddle2", "paddle3", "paddle4",
                   "touchpad")

    def __init__(self):
        import sdl2
        self.sdl = sdl2
        sdl2.SDL_SetHint(sdl2.SDL_HINT_NO_SIGNAL_HANDLERS, b"1")
        sdl2.SDL_SetHint(sdl2.SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, b"1")
        if sdl2.SDL_InitSubSystem(sdl2.SDL_INIT_GAMECONTROLLER) != 0:
            raise RuntimeError(sdl2.SDL_GetError().decode("utf-8", "replace"))
        self.initialized = True
        self.joystick = None
        self.pad = None
        self.instance = None
        self.counts = (0, 0, 0)
        self.extra = {}  # instance -> (joystick, counts): pedal and shifter devices (#272)
        # Linux devices SDL2 leaves out (pedals, shifters), read from evdev (evdev_joysticks.py):
        # instance -> EvdevJoystick, and the event nodes they were found among.
        self.evdev = {}
        self.evdev_nodes = None
        self.evdev_main = None

    def scan_evdev(self, listed):
        if not sys.platform.startswith("linux"):
            return []
        from . import evdev_joysticks
        try:
            nodes = sorted(n for n in os.listdir("/dev/input") if n.startswith("event"))
        except OSError:
            return []
        if nodes != self.evdev_nodes:  # plugged in or out: look again
            self.evdev_nodes = nodes
            keep = {i for i in (self.instance, *self.extra) if i is not None and i < 0}
            for instance in [i for i in self.evdev if i not in keep]:
                self.evdev.pop(instance).close()
            found = evdev_joysticks.scan(listed)
            for number, joy in found.items():
                instance = evdev_joysticks.INSTANCE_BASE - number
                if instance in self.evdev:
                    joy.close()
                else:
                    self.evdev[instance] = joy
        return [DeviceInfo(i, j.name, "", j.vid, j.pid, False, False)
                for i, j in sorted(self.evdev.items(), reverse=True) if j.fd is not None]

    def drop_evdev(self, instance):
        joy = self.evdev.pop(instance, None)
        if joy is not None:
            joy.close()

    def devices(self):
        s = self.sdl
        s.SDL_PumpEvents()
        out = []
        for i in range(s.SDL_NumJoysticks()):
            buf = ctypes.create_string_buffer(33)
            s.SDL_JoystickGetGUIDString(s.SDL_JoystickGetDeviceGUID(i), buf, 33)
            name = s.SDL_JoystickNameForIndex(i)
            out.append(DeviceInfo(
                s.SDL_JoystickGetDeviceInstanceID(i),
                name.decode("utf-8", "replace") if name else f"Joystick {i}",
                buf.value.decode("ascii", "replace"),
                s.SDL_JoystickGetDeviceVendor(i), s.SDL_JoystickGetDeviceProduct(i),
                bool(s.SDL_IsGameController(i)),
                s.SDL_JoystickGetDeviceType(i) == s.SDL_JOYSTICK_TYPE_WHEEL))
        return out + self.scan_evdev([(d.name, d.vid, d.pid) for d in out])

    def open(self, instance):
        """Open the joystick with this instance id (closing the previous one)."""
        s = self.sdl
        if instance == self.instance and self.joystick and s.SDL_JoystickGetAttached(self.joystick):
            return True
        if instance == self.instance and self.evdev_main is not None:
            return True
        self.release()
        if instance is not None and instance < 0 and instance in self.evdev:
            self.evdev_main = self.evdev[instance]
            self.instance = instance
            self.counts = self.evdev_main.counts
            return True
        for i in range(s.SDL_NumJoysticks()):
            if s.SDL_JoystickGetDeviceInstanceID(i) != instance:
                continue
            if s.SDL_IsGameController(i):
                self.pad = s.SDL_GameControllerOpen(i)
                self.joystick = s.SDL_GameControllerGetJoystick(self.pad) if self.pad else None
            if not self.joystick:
                self.joystick = s.SDL_JoystickOpen(i)
            if self.joystick:
                self.instance = instance
                j = self.joystick
                self.counts = (s.SDL_JoystickNumAxes(j), s.SDL_JoystickNumButtons(j),
                               s.SDL_JoystickNumHats(j))
                return True
        return False

    def read(self):
        """The open joystick's state (bindings.Snapshot), or None when it is gone."""
        from .bindings import Snapshot
        s = self.sdl
        if self.evdev_main is not None:
            got = self.evdev_main.read()
            if got is None:
                self.drop_evdev(self.instance)
                self.release()
                return None
            return Snapshot(*got)
        if not self.joystick:
            return None
        s.SDL_PumpEvents()
        s.SDL_JoystickUpdate()
        if not s.SDL_JoystickGetAttached(self.joystick):
            self.release()
            return None
        j = self.joystick
        axes, buttons, hats = self.counts
        pad_buttons, pad_axes = frozenset(), ()
        if self.pad:
            b = {"a": s.SDL_CONTROLLER_BUTTON_A, "b": s.SDL_CONTROLLER_BUTTON_B,
                 "x": s.SDL_CONTROLLER_BUTTON_X, "y": s.SDL_CONTROLLER_BUTTON_Y,
                 "back": s.SDL_CONTROLLER_BUTTON_BACK, "guide": s.SDL_CONTROLLER_BUTTON_GUIDE,
                 "start": s.SDL_CONTROLLER_BUTTON_START, "ls": s.SDL_CONTROLLER_BUTTON_LEFTSTICK,
                 "rs": s.SDL_CONTROLLER_BUTTON_RIGHTSTICK,
                 "lb": s.SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                 "rb": s.SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                 "up": s.SDL_CONTROLLER_BUTTON_DPAD_UP, "down": s.SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                 "left": s.SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                 "right": s.SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                 "misc1": s.SDL_CONTROLLER_BUTTON_MISC1,
                 "paddle1": s.SDL_CONTROLLER_BUTTON_PADDLE1,
                 "paddle2": s.SDL_CONTROLLER_BUTTON_PADDLE2,
                 "paddle3": s.SDL_CONTROLLER_BUTTON_PADDLE3,
                 "paddle4": s.SDL_CONTROLLER_BUTTON_PADDLE4,
                 "touchpad": s.SDL_CONTROLLER_BUTTON_TOUCHPAD}
            pad_buttons = frozenset(n for n, v in b.items() if s.SDL_GameControllerGetButton(self.pad, v))

            def axis(a, sign=1):
                v = s.SDL_GameControllerGetAxis(self.pad, a) * sign
                return max(-1.0, min(1.0, v / 32767.0))
            pad_axes = (axis(s.SDL_CONTROLLER_AXIS_LEFTX), axis(s.SDL_CONTROLLER_AXIS_LEFTY, -1),
                        axis(s.SDL_CONTROLLER_AXIS_RIGHTX), axis(s.SDL_CONTROLLER_AXIS_RIGHTY, -1),
                        max(0.0, axis(s.SDL_CONTROLLER_AXIS_TRIGGERLEFT)),
                        max(0.0, axis(s.SDL_CONTROLLER_AXIS_TRIGGERRIGHT)))
        return Snapshot(tuple(s.SDL_JoystickGetAxis(j, n) for n in range(axes)),
                        tuple(bool(s.SDL_JoystickGetButton(j, n)) for n in range(buttons)),
                        tuple(s.SDL_JoystickGetHat(j, n) for n in range(hats)),
                        pad_buttons, pad_axes)

    def watch(self, instances):
        """Keep these joysticks open too, next to the one `open` holds: a wheel's pedals and
        shifter on their own USB devices bind with it (#272)."""
        s = self.sdl
        for instance in list(self.extra):
            joystick = self.extra[instance][0]
            if instance < 0:  # evdev: kept open in self.evdev
                if instance not in instances:
                    del self.extra[instance]
                continue
            if instance not in instances or not s.SDL_JoystickGetAttached(joystick):
                s.SDL_JoystickClose(joystick)
                del self.extra[instance]
        for instance in instances:
            if instance == self.instance or instance in self.extra:
                continue
            if instance < 0:
                if instance in self.evdev:
                    self.extra[instance] = (self.evdev[instance], self.evdev[instance].counts)
                continue
            for i in range(s.SDL_NumJoysticks()):
                if s.SDL_JoystickGetDeviceInstanceID(i) != instance:
                    continue
                j = s.SDL_JoystickOpen(i)
                if j:
                    self.extra[instance] = (j, (s.SDL_JoystickNumAxes(j), s.SDL_JoystickNumButtons(j),
                                                s.SDL_JoystickNumHats(j)))
                break

    def extra_counts(self, instance):
        return self.extra[instance][1] if instance in self.extra else (0, 0, 0)

    def read_extra(self, instance):
        """A watched joystick's raw state (bindings.Snapshot), or None when it is gone."""
        from .bindings import Snapshot
        s = self.sdl
        if instance not in self.extra:
            return None
        if instance < 0:
            got = self.extra[instance][0].read()
            if got is None:
                del self.extra[instance]
                self.drop_evdev(instance)
                return None
            return Snapshot(*got)
        s.SDL_PumpEvents()
        s.SDL_JoystickUpdate()
        j, (axes, buttons, hats) = self.extra[instance]
        if not s.SDL_JoystickGetAttached(j):
            s.SDL_JoystickClose(j)
            del self.extra[instance]
            return None
        return Snapshot(tuple(s.SDL_JoystickGetAxis(j, n) for n in range(axes)),
                        tuple(bool(s.SDL_JoystickGetButton(j, n)) for n in range(buttons)),
                        tuple(s.SDL_JoystickGetHat(j, n) for n in range(hats)))

    def release(self):
        s = self.sdl
        self.evdev_main = None
        if self.pad:
            s.SDL_GameControllerClose(self.pad)
        elif self.joystick:
            s.SDL_JoystickClose(self.joystick)
        self.pad = self.joystick = self.instance = None
        self.counts = (0, 0, 0)

    def close(self):
        self.watch(())
        self.release()
        for joy in self.evdev.values():
            joy.close()
        self.evdev, self.evdev_nodes = {}, None
        if self.initialized:
            self.sdl.SDL_QuitSubSystem(self.sdl.SDL_INIT_GAMECONTROLLER)
            self.initialized = False
