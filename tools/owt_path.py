#!/usr/bin/env python3
"""owt_path.py ROUTE.owt OUT [LAT_ACCEL] [BRAKE] [VMAX]: an AI route as a path for autodrive.py.

A test tool. The game's AI routes (`media/AIOpenWorld/<track>/Ribbon_00/route_NNN.owt`;
NNN is the `RouteId` of the track in gamedb.slt's Tracks table) are "OWTM" files: a
0x20-byte header (u32 record count at 0x10, big-endian), then 48-byte records: the road's
center (x, y, z, pad), a vector across the road (x, y, z, pad; 5-8 m long), a factor in
-1..1 (taken here as the AI's line: center + factor * vector) and three zero words.
Points are about 2 m apart. A route can run past both ends of its stage.

Writes "x z speed" per line (AD_PATH_LOAD with AD_OPEN=1 of tools/autodrive.py): that
line (AD_LINE=0 for the center), every 2nd point, with a speed from the curvature (LAT_ACCEL
m/s^2, default 4.5) limited ahead of each bend so the car can slow down in time (BRAKE
m/s^2, default 3.5), at most VMAX m/s (default 32).
"""
import math, struct, sys

src, out = sys.argv[1], sys.argv[2]
LAT = float(sys.argv[3]) if len(sys.argv) > 3 else 4.5
BRAKE = float(sys.argv[4]) if len(sys.argv) > 4 else 3.5
VMAX = float(sys.argv[5]) if len(sys.argv) > 5 else 32.0
LINE = float(__import__("os").environ.get("AD_LINE", "1"))

b = open(src, "rb").read()
if b[:4] != b"OWTM":
    sys.exit("not an OWTM file")
n = struct.unpack(">I", b[0x10:0x14])[0]
pts = []
for i in range(0, n, 2):
    r = struct.unpack(">12f", b[0x20 + 48 * i:0x20 + 48 * (i + 1)])
    pts.append((r[0] + LINE * r[8] * r[4], r[2] + LINE * r[8] * r[6]))


def heading(a, b):
    return math.atan2(b[1] - a[1], b[0] - a[0])


N = len(pts)
W = 5  # points on each side (about 20 m) for the curvature
v = []
for i in range(N):
    a, c = max(0, i - W), min(N - 1, i + W)
    if c - a < 2:
        v.append(VMAX)
        continue
    m = (a + c) // 2
    turn = heading(pts[m], pts[c]) - heading(pts[a], pts[m])
    turn = (turn + math.pi) % (2 * math.pi) - math.pi
    dist = sum(math.hypot(pts[k + 1][0] - pts[k][0], pts[k + 1][1] - pts[k][1]) for k in range(a, c))
    k = abs(turn) / max(dist, 1.0)  # 1/radius
    v.append(min(VMAX, math.sqrt(LAT / k)) if k > 1e-6 else VMAX)
for i in range(N - 2, -1, -1):  # slow down before the bends
    ds = math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
    v[i] = min(v[i], math.sqrt(v[i + 1] ** 2 + 2 * BRAKE * ds))
with open(out, "w") as f:
    f.writelines("%.2f %.2f %.2f\n" % (p[0], p[1], s) for p, s in zip(pts, v))
length = sum(math.hypot(pts[k + 1][0] - pts[k][0], pts[k + 1][1] - pts[k][1]) for k in range(N - 1))
print("%d points, %.0f m, %.0f s at the path's speeds" % (
    N, length, sum(math.hypot(pts[k + 1][0] - pts[k][0], pts[k + 1][1] - pts[k][1]) / v[k] for k in range(N - 1))))
