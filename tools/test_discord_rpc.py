"""Exercise the native RPC client against a private local fixture, never real Discord.

Usage: python tools/test_discord_rpc.py --exe PATH/forza[.exe]
Build the probe with forza-recomp/tests/discord/CMakeLists.txt first.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

PREFIX = "forza-discord-test-ipc-"
EXE = None


class Endpoint:
    def __init__(self, root):
        self.root = root
        self.connection = None
        self.listener = None
        if os.name == "nt":
            from ctypes import wintypes
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
            self.kernel.CreateNamedPipeW.restype = wintypes.HANDLE
            for name in ("ConnectNamedPipe", "DisconnectNamedPipe", "CloseHandle"):
                getattr(self.kernel, name).argtypes = [wintypes.HANDLE] + ([wintypes.LPVOID] if name == "ConnectNamedPipe" else [])
            for name in ("ReadFile", "WriteFile"):
                getattr(self.kernel, name).argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                                                     ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
            self.handle = self.kernel.CreateNamedPipeW(r"\\.\pipe" + "\\" + PREFIX + "0", 3, 0, 1,
                                                       65536, 65536, 1000, None)
            if self.handle == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            self.path = root / (PREFIX + "0")
            self.listener = socket.socket(socket.AF_UNIX)
            self.listener.settimeout(12)
            self.listener.bind(str(self.path))
            self.listener.listen(1)

    def accept(self):
        if os.name == "nt":
            if not self.kernel.ConnectNamedPipe(self.handle, None) and ctypes.get_last_error() != 535:
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            self.connection, _ = self.listener.accept()
            self.connection.settimeout(12)

    def read(self, size):
        result = b""
        while len(result) < size:
            if os.name == "nt":
                count = ctypes.c_ulong()
                buffer = ctypes.create_string_buffer(size - len(result))
                if not self.kernel.ReadFile(self.handle, buffer, len(buffer), ctypes.byref(count), None):
                    raise EOFError()
                data = buffer.raw[:count.value]
            else:
                data = self.connection.recv(size - len(result))
            if not data:
                raise EOFError()
            result += data
        return result

    def write(self, data):
        if os.name == "nt":
            count = ctypes.c_ulong()
            buffer = ctypes.create_string_buffer(data)
            if not self.kernel.WriteFile(self.handle, buffer, len(data), ctypes.byref(count), None):
                raise EOFError()
            if count.value != len(data):
                raise EOFError()
        else:
            self.connection.sendall(data)

    def frame(self):
        opcode, length = struct.unpack("<II", self.read(8))
        if length > 65536:
            raise ValueError("oversized fixture frame")
        payload = self.read(length)
        return opcode, payload

    def send(self, opcode, payload, fragmented=False):
        raw = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        frame = struct.pack("<II", opcode, len(raw)) + raw
        if fragmented:
            self.write(frame[:4]); time.sleep(.05)
            self.write(frame[4:11]); time.sleep(.05)
            self.write(frame[11:])
        else:
            self.write(frame)

    def close(self):
        if os.name == "nt":
            if self.handle:
                self.kernel.DisconnectNamedPipe(self.handle)
                self.kernel.CloseHandle(self.handle)
                self.handle = None
        else:
            if self.connection:
                self.connection.close()
            if self.listener:
                self.listener.close()
            self.path.unlink(missing_ok=True)


class DiscordTests(unittest.TestCase):
    def exercise(self, reconnect=False):
        messages, errors, endpoints = [], [], []
        with tempfile.TemporaryDirectory(prefix="forza-discord-") as temp:
            root = Path(temp)
            first = Endpoint(root)
            endpoints.append(first)

            def server():
                try:
                    for index in range(2 if reconnect else 1):
                        endpoint = first if index == 0 else Endpoint(root)
                        if index:
                            endpoints.append(endpoint)
                        endpoint.accept()
                        opcode, payload = endpoint.frame()
                        messages.append(("handshake", json.loads(payload)))
                        self.assertEqual(opcode, 0)
                        endpoint.send(1, {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1,
                            "user": {"id": "1234", "username": "fixture", "discriminator": "0000", "avatar": ""}}},
                            fragmented=True)
                        endpoint.send(3, b"fixture-ping")
                        while True:
                            opcode, payload = endpoint.frame()
                            if opcode == 4:
                                messages.append(("pong", payload))
                                continue
                            body = json.loads(payload)
                            messages.append(("frame", body))
                            if body.get("cmd") == "SET_ACTIVITY":
                                endpoint.send(1, {"cmd": "SET_ACTIVITY", "nonce": body["nonce"], "data": {}})
                                if reconnect and index == 0 and body.get("args", {}).get("activity"):
                                    endpoint.close()
                                    break
                                if body.get("args", {}).get("activity"):
                                    endpoint.send(1, {"cmd": "DISPATCH", "evt": "ACTIVITY_JOIN",
                                                      "data": {"secret": "fixture-observed-activity"}})
                except EOFError:
                    pass
                except BaseException as error:
                    errors.append(error)
                finally:
                    for endpoint in endpoints:
                        endpoint.close()

            thread = threading.Thread(target=server, daemon=True)
            thread.start()
            env = dict(os.environ, XDG_RUNTIME_DIR=str(root))
            try:
                child = subprocess.run([str(EXE), "123456789012345678"], env=env,
                                       capture_output=True, text=True, timeout=15)
            except subprocess.TimeoutExpired as error:
                self.fail(f"IPC client timed out: stdout={error.stdout!r}, "
                          f"stderr={error.stderr!r}, messages={messages!r}, errors={errors!r}")
            finally:
                for endpoint in endpoints:
                    endpoint.close()
                thread.join(2)
            self.assertFalse(thread.is_alive(), "fixture thread did not stop")
            self.assertFalse(errors, f"{errors!r}; stdout={child.stdout!r}; stderr={child.stderr!r}; "
                             f"messages={messages!r}")
            self.assertEqual(child.returncode, 0, child.stdout + child.stderr)
            self.assertIn("READY", child.stdout)
            handshakes = [body for kind, body in messages if kind == "handshake"]
            self.assertEqual(len(handshakes), 2 if reconnect else 1)
            self.assertTrue(all(h["client_id"] == "123456789012345678" and h["v"] == 1 for h in handshakes))
            self.assertIn(("pong", b"fixture-ping"), messages)
            frames = [body for kind, body in messages if kind == "frame" and body.get("cmd") == "SET_ACTIVITY"]
            activities = [f["args"]["activity"] for f in frames if f.get("args", {}).get("activity")]
            self.assertGreaterEqual(len(activities), 2 if reconnect else 1)
            self.assertEqual(activities[-1]["details"], "Exploring Colorado")
            self.assertEqual(activities[-1]["state"], "Free roam")
            self.assertNotIn("secrets", activities[-1])
            self.assertNotIn("party", activities[-1])
            self.assertTrue(any(f["args"].get("activity") is None for f in frames), "clear was not received")

    def test_handshake_activity_fragmented_frames_ping_and_clear(self):
        self.exercise()

    def test_disconnect_reconnect_resends_latest_activity(self):
        self.exercise(reconnect=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    EXE = args.exe.resolve()
    unittest.main(argv=[__file__] + remaining)
