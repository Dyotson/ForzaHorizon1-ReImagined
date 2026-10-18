"""SDL controller input, independent of Qt. Only the launcher UI initializes SDL."""
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
