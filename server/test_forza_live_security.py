"""Abuse tests for the public server: operator endpoints, malformed input, limits."""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

import forza_limits
import forza_live_lsp as lsp
import forza_xtsw as x

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"RLV1"


def free_port(kind=socket.SOCK_DGRAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def get(url, headers=None):
    try:
        req = urllib.request.Request(url, headers=headers or {})
        return urllib.request.urlopen(req, timeout=3).status
    except urllib.error.HTTPError as e:
        return e.code


class AdminTest(unittest.TestCase):
    def test_is_admin(self):
        lsp.ADMIN_TOKEN = ""
        self.assertTrue(lsp.is_admin(("127.0.0.1", 1), {}, {}))
        self.assertFalse(lsp.is_admin(("203.0.113.9", 1), {}, {}))
        self.assertFalse(lsp.is_admin(("192.168.1.5", 1), {}, {}))
        self.assertFalse(lsp.is_admin(("127.0.0.1", 1), {"x-forwarded-for": "203.0.113.9"}, {}))
        lsp.ADMIN_TOKEN = "s3cret"
        try:
            self.assertTrue(lsp.is_admin(("203.0.113.9", 1), {"x-admin-token": "s3cret"}, {}))
            self.assertTrue(lsp.is_admin(("203.0.113.9", 1), {}, {"token": ["s3cret"]}))
            self.assertFalse(lsp.is_admin(("203.0.113.9", 1), {"x-admin-token": "nope"}, {}))
        finally:
            lsp.ADMIN_TOKEN = ""


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port, cls.http = free_port(), free_port(socket.SOCK_STREAM)
        cls.data = tempfile.TemporaryDirectory()
        env = dict(os.environ, FORZA_ADMIN_TOKEN="tok123")
        cls.proc = subprocess.Popen(
            # Windows venv's python.exe is a launcher; terminating it leaves its
            # child server alive, now holding events.sqlite3 open at cleanup.
            [getattr(sys, "_base_executable", sys.executable), os.path.join(HERE, "forza_live_server.py"), "--host", "127.0.0.1",
             "--port", str(cls.port), "--http-port", str(cls.http), "--data-dir", cls.data.name],
            stderr=subprocess.DEVNULL, env=env)
        time.sleep(0.8)
        cls.base = "http://127.0.0.1:%d" % cls.http

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()
        cls.data.cleanup()

    def test_admin_endpoints_refuse_proxied_requests(self):
        proxied = {"X-Forwarded-For": "203.0.113.9"}
        for path in ("/gift?to=x&credits=1", "/notify?to=x", "/world/event?session=all"):
            self.assertEqual(get(self.base + path, proxied), 403, path)
            self.assertEqual(get(self.base + path + "&token=wrong", proxied), 403, path)
            self.assertNotEqual(get(self.base + path + "&token=tok123", proxied), 403, path)
        # from this machine (no proxy header) without a token
        self.assertEqual(get(self.base + "/gift?to=nobody&credits=1"), 404)

    def test_status_page_has_no_addresses(self):
        text = urllib.request.urlopen(self.base + "/status", timeout=3).read().decode()
        self.assertNotRegex(text, r"\d+\.\d+\.\d+\.\d+")

    def test_garbage_udp_and_http(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for kind in range(0, 40):
            for body in (b"", b"\xff", b"\x00" * 3, b"\xff" * 40):
                sock.sendto(MAGIC + bytes([kind]) + body, ("127.0.0.1", self.port))
        sock.close()
        for raw in (b"\xff\xfe\r\n\r\n", b"POST / HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n",
                    b"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", b"A" * 100000 + b"\r\n\r\n"):
            s = socket.create_connection(("127.0.0.1", self.http), timeout=3)
            s.sendall(raw)
            s.settimeout(2)
            try:
                s.recv(100)
            except (socket.timeout, ConnectionError):
                pass
            s.close()
        self.assertEqual(get(self.base + "/"), 200)

    def test_welcome_is_small_and_peers_per_ip_capped(self):
        welcomes = 0
        for i in range(24):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(0.5)
            hello = MAGIC + b"\x01" + struct.pack(">HQQIH", 1, 1, 0x0009000000000300 + i, 0, 1) + b"\0\2Pn"
            s.sendto(hello, ("127.0.0.1", self.port))
            try:
                reply, _ = s.recvfrom(4096)
                if reply[4] == 2:
                    welcomes += 1
                    self.assertLess(len(reply), 3 * len(hello))
            except socket.timeout:
                pass
            s.close()
        self.assertEqual(welcomes, 16)


class LimitsTest(unittest.TestCase):
    def test_blob_cap(self):
        with self.assertRaises(ValueError):
            forza_limits.accept(forza_limits.MAX_BLOB + 1)

    def test_array_length_is_bounded(self):
        data = struct.pack(">I", 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            x.decode(x.ARR(x.RAW(4)), data)


if __name__ == "__main__":
    unittest.main()
