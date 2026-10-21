"""Run via the frozen launcher's --run-script to verify SDL is actually bundled.

The report is required because the Windows GUI executable has no console.
No physical controller, game files or user configuration are needed.
"""
import sys
from pathlib import Path

import sdl2
from forza_launcher.gamepad import SDLController


def main(report):
    backend = SDLController()
    index = sdl2.SDL_JoystickAttachVirtual(sdl2.SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 21, 0)
    joystick = None
    try:
        if index < 0:
            raise RuntimeError(sdl2.SDL_GetError())
        joystick = sdl2.SDL_JoystickOpen(index)
        backend.controller = sdl2.SDL_GameControllerOpen(index)
        if not joystick or not backend.controller:
            raise RuntimeError(sdl2.SDL_GetError())
        sdl2.SDL_JoystickSetVirtualButton(joystick, sdl2.SDL_CONTROLLER_BUTTON_A, 1)
        sdl2.SDL_JoystickSetVirtualAxis(joystick, sdl2.SDL_CONTROLLER_AXIS_LEFTX, 24000)
        state = backend.read()
        if "accept" not in state.buttons or state.x != 24000:
            raise RuntimeError(f"Incorrect controller mapping: {state}")
        Path(report).write_text("SDL controller smoke test passed\n", encoding="utf-8")
    finally:
        if joystick:
            sdl2.SDL_JoystickClose(joystick)
        if index >= 0:
            sdl2.SDL_JoystickDetachVirtual(index)
        backend.close()


if __name__ == "__main__":
    main(sys.argv[1])
