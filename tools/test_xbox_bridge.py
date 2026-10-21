"""Exercise native FH1X guards/acks with telemetry_probe.exe --xbox.

Usage: python tools/test_xbox_bridge.py PATH/telemetry_probe.exe
Works without a controller, game data or initialized SDK input system.
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
from forza_launcher.haptics.xbox import COMMAND, ACK


def main():
    exe = Path(sys.argv[1]).resolve()
    token = secrets.randbits(64) or 1
    decoder = Decoder(token)
    acks = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
        receiver.bind(('127.0.0.1', 0))
        receiver.settimeout(.1)
        env = dict(os.environ, FORZA_TELEMETRY_PORT=str(receiver.getsockname()[1]),
                   FORZA_TELEMETRY_TOKEN=f'{token:016x}')
        process = subprocess.Popen([str(exe), '--xbox'], cwd=exe.parent, env=env)
        deadline = time.monotonic()+15
        try:
            while time.monotonic() < deadline:
                try:
                    data, peer = receiver.recvfrom(4096)
                except socket.timeout:
                    if process.poll() is not None: break
                    continue
                frame = decoder.decode(data)
                if frame is not None:
                    # Invalid token, reserved bits, old echoed sample and old command
                    # sequences must never be acknowledged or applied.
                    def command(seq, sample=frame.sequence, nonce=token, reserved=0):
                        return COMMAND.pack(b'FH1X', 1, 32, nonce, seq, sample, 10000, 20000, reserved)
                    receiver.sendto(command(100000+frame.sequence, nonce=token ^ 1), peer)
                    receiver.sendto(command(100000+frame.sequence, reserved=1), peer)
                    receiver.sendto(command(100000+frame.sequence, sample=0xffff0000), peer)
                    receiver.sendto(command(0), peer)  # Replay after the first valid command.
                    receiver.sendto(command(frame.sequence+1), peer)
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger:
                        stranger.sendto(command(90000+frame.sequence), peer)
                else:
                    assert len(data) == ACK.size, 'Unexpected native output'
                    magic, version, size, nonce, seq, result = ACK.unpack(data)
                    assert (magic, version, size, nonce) == (b'FH1A', 1, 24, token)
                    assert 0 <= seq < 100, f'Invalid/replayed command acknowledged: {seq}'
                    assert result in (-2, 0), f'Unexpected output without SDK input initialization: {result}'
                    acks.append(seq)
            assert process.wait(timeout=1) == 0
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
    assert len(acks) > 10, acks
    assert len(acks) == len(set(acks)), 'Duplicate/replayed command acknowledged'
    assert 16 not in acks, 'Command queued before the probe pause was not expired'
    print('PASS: native Xbox command/ack, nonce, reserved fields, sender port, sequence and sample freshness guards')


if __name__ == '__main__': main()
