"""Check a compiled Windows telemetry_probe.exe using sparse synthetic guest memory.

Usage: python tools/test_telemetry_bridge.py PATH/telemetry_probe.exe
The probe must be built with the same FORZA_TITLE_UPDATE as telemetry.cpp and
have the runtime DLL beside it. It needs no game files, save or controller.
"""
from pathlib import Path
import os
import secrets
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
from forza_launcher.haptics.telemetry import Decoder


def main():
    executable = Path(sys.argv[1]).resolve()
    frames = []
    token = secrets.randbits(64) or 1
    decoder = Decoder(token)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind(('127.0.0.1', 0))
        receiver.settimeout(.1)
        env = dict(os.environ, FORZA_TELEMETRY_PORT=str(receiver.getsockname()[1]),
                   FORZA_TELEMETRY_TOKEN=f'{token:016x}')
        process = subprocess.Popen([str(executable)], cwd=executable.parent, env=env)
        deadline = time.monotonic()+15
        try:
            while time.monotonic() < deadline:
                try:
                    data, _ = receiver.recvfrom(4096)
                    frame = decoder.decode(data)
                    assert frame is not None, 'Invalid native packet'
                    frames.append(frame)
                except socket.timeout:
                    if process.poll() is not None:
                        break
            assert process.wait(timeout=1) == 0
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
    assert len(frames) == 6, f'Expected six native packets, got {len(frames)}'
    assert [f.flags for f in frames] == [3, 7, 15, 1, 2, 2]
    assert [f.active for f in frames] == [True, True, True, False, False, False]
    assert frames[0].brake == 255 and frames[0].throttle == 128 and frames[0].steer == -128
    print('PASS: native FH1T input mapping, derivatives, teleport reset, remote-car and unreadable-pointer rejection')


if __name__ == '__main__':
    main()
