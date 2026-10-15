#!/usr/bin/env python3
"""Check a Forza Live server from outside: TCP status page and a UDP HELLO -> WELCOME.

Usage: server_probe.py [host[:port]]   (default dyotserver.ddns.net:21200)
Registers as a throwaway player in the room "probe" (never shown to players) and says BYE.
Exit status 0 when both answer.
"""
import os, socket, struct, sys, time, urllib.request

MAGIC, HELLO, WELCOME, ERROR, BYE = b"RLV1", 1, 2, 3, 10
host, _, port = (sys.argv[1] if len(sys.argv) > 1 else "dyotserver.ddns.net:21200").partition(":")
port = int(port or 21200)
ok = True

try:
    with urllib.request.urlopen(f"http://{host}:{port + 1}/", timeout=10) as r:
        print(f"PASS tcp {port + 1}: status page {r.status}")
except Exception as e:
    ok = False
    print(f"FAIL tcp {port + 1}: {e}")

token, xuid = int.from_bytes(os.urandom(8), "big"), 0x0009000000000000 | int.from_bytes(os.urandom(4), "big")
body = struct.pack(">HQQIH", 1, token, xuid, 0, 0) + bytes([5]) + b"probe" + bytes([5]) + b"Probe"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
addr = (socket.gethostbyname(host), port)
answer = None
for attempt in range(3):
    s.sendto(MAGIC + bytes([HELLO]) + body, addr)
    try:
        data, _ = s.recvfrom(4096)
        if data[:4] == MAGIC:
            answer = data[4]
            break
    except socket.timeout:
        continue
if answer == WELCOME:
    print(f"PASS udp {port}: WELCOME from {addr[0]}")
    s.sendto(MAGIC + bytes([BYE]), addr)
else:
    ok = False
    print(f"FAIL udp {port}: " + ("no answer in 3 tries" if answer is None else f"answer kind {answer}"))
sys.exit(0 if ok else 1)
