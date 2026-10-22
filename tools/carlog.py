#!/usr/bin/env python3
"""carlog.py PID SECS OUT: sample every car's position from a running game's guest memory
(the car list as tools/autodrive.py reads it) at about 250 Hz for SECS seconds. OUT gets
lines "t kind slot x z" (t: host monotonic seconds, the same clock for every game on the
machine; kind 0 is the player's car, 1 a CPU driver, 2 another console's car or a ghost),
written when a car moved. tools/frametime_report.py reads it (docs/framerate.md)."""
import math, struct, sys, time

pid, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
mem = open("/proc/%s/mem" % pid, "rb")
# The car list's root: 0x832DF024 in the base executable, 0x833CAAE4 in Title Update 4 (a TU4
# game runs with --update_data_root), as in tools/autodrive.py.
CAR_ROOT = 0x833CAAE4 if b"--update_data_root" in open("/proc/%s/cmdline" % pid, "rb").read() else 0x832DF024


def rd(a, n):
    mem.seek(0x100000000 + a)
    return mem.read(n)


def u32(a):
    return struct.unpack(">I", rd(a, 4))[0]


def ok(a):
    return 0x00100000 <= a < 0xF0000000


def cars():
    root = u32(CAR_ROOT)
    if not ok(root):
        return []
    holder = u32(root + 4)
    if not ok(holder):
        return []
    lst = u32(holder + 4)
    if not ok(lst):
        return []
    first, end = u32(lst + 0x80), u32(lst + 0x84)
    if not ok(first) or end < first or end - first > 256:
        return []
    res = []
    for slot in range(first, end, 4):
        v = u32(slot)
        if not ok(v):
            continue
        kind = u32(v + 0x2DD0)
        car = u32(v + 0x2D90)
        if not ok(car):
            continue
        p = struct.unpack(">3f", rd(car - 0xA20, 12))
        if all(math.isfinite(c) and abs(c) < 1e5 for c in p):
            res.append((kind, slot, p[0], p[2]))
    return res


f = open(out, "w")
last = {}
end = time.monotonic() + secs
while time.monotonic() < end:
    t = time.monotonic()
    try:
        cs = cars()
    except OSError:
        break
    for kind, slot, x, z in cs:
        if last.get(slot) != (x, z):
            last[slot] = (x, z)
            f.write("%.5f %d %X %.3f %.3f\n" % (t, kind, slot, x, z))
    time.sleep(0.004)
f.close()
