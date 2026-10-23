#!/usr/bin/env python3
"""Build the website's illustrated map of Colorado (#316) from players' driven lines.

The roads are the lines players drove: the Rivals ghosts the Forza Live server keeps
(DATA_DIR/ghosts/<id>.bin) and, optionally, drives of our own logged with tools/carlog.py.
Nothing of the game's map, terrain or road data is used. The paths are merged (a stretch
already drawn within MERGE_M is not drawn again), simplified (Douglas-Peucker) and written
as a few SVG paths. The terrain is our own artwork (tools/map_terrain.json: approximate
regions, the reservoir and label spots in world metres); the landmass, the regions' soft
shapes and the peaks and mesas on them are generated from it and from the roads. The
landmark icons are drawn here. Rerun it as more ghosts arrive:

  tools/build_map_roads.py --ghosts DATA_DIR/ghosts [--carlog DRIVE.log ...] \\
      --points site/data/map_points.json --out site/images/colorado-map.svg

A ghost (what the game uploads with a Rivals lap, kept as it is by the server): a 16-byte
header of big-endian words (the first block's size + 8, the size of the rest, the first
block's compressed and uncompressed sizes), the first block (raw deflate: the car's setup),
then chunks of [compressed size, uncompressed size (4096 at most), raw deflate data]. The
chunks hold a stream of records; a car-state record is 0x40, the car's 16-bit object id and
its position as three big-endian floats (x, y, z in metres), among others (times, inputs).

Coordinates: the game's x grows east and z north; the SVG has 1 unit = UNIT metres, north up.
The colours are CSS custom properties (--map-*) with fallbacks, so a page that inlines the
SVG styles it for light and dark (site/style.css).
"""
import argparse
import json
import math
import os
import random
import struct
import sys
import zlib

UNIT = 4.0          # metres per SVG unit
STEP_M = 8.0        # resampling step of a path
MERGE_M = 16.0      # a stretch within this of a drawn road is the same road
GAP_M = 80.0        # a jump longer than this splits a path (a reset, a rewind)
MIN_RUN = 6         # samples: shorter new stretches are dropped (unless they join roads)
SIMPLIFY_M = 4.0    # Douglas-Peucker tolerance
MAIN_USES = 3       # a stretch driven by this many paths is a main road


# ---- ghosts -------------------------------------------------------------------------------

def ghost_stream(data):
    """The decompressed record stream of a ghost; ValueError when it is not one."""
    if len(data) < 16:
        raise ValueError("too short")
    first, rest, comp1, _ = struct.unpack(">4I", data[:16])
    if first != comp1 + 8 or 16 + comp1 + rest != len(data):
        raise ValueError("not a ghost header")
    pos, out = 16 + comp1, []
    while pos < len(data):
        if pos + 8 > len(data):
            raise ValueError("truncated chunk")
        comp, size = struct.unpack(">II", data[pos:pos + 8])
        pos += 8
        if comp == 0 or pos + comp > len(data) or size > 1 << 20:
            raise ValueError("bad chunk")
        try:
            chunk = zlib.decompressobj(-15).decompress(data[pos:pos + comp])
        except zlib.error as e:
            raise ValueError("bad chunk: %s" % e)
        out.append(chunk[:size])
        pos += comp
    return b"".join(out)


def stream_path(s):
    """[(x, y, z)] of the car-state records of a record stream: 0x40, the object id, three
    floats. The ghost car's id is the most frequent one; a record far from the previous one
    is kept only when the path goes on from it (a stray 0x40 inside another record does not)."""
    cand = []
    for i in range(len(s) - 15):
        if s[i] != 0x40:
            continue
        x, y, z = struct.unpack(">3f", s[i + 3:i + 15])
        if all(math.isfinite(v) for v in (x, y, z)) and abs(x) < 20000 and abs(z) < 20000 \
                and -1000 < y < 5000 and (abs(x) > 1 or abs(z) > 1):
            cand.append((i, s[i + 1:i + 3], x, y, z))
    if not cand:
        return []
    counts = {}
    for c in cand:
        counts[c[1]] = counts.get(c[1], 0) + 1
    car = max(counts, key=counts.get)
    mine = [c for c in cand if c[1] == car]
    out, last = [], -15
    for k, (i, _, x, y, z) in enumerate(mine):
        if i < last + 15:
            continue
        if out and math.hypot(x - out[-1][0], z - out[-1][2]) > GAP_M:
            # A jump: real (a rewind, a reset) when the next record goes on from here, else a
            # stray match inside another record.
            nxt = mine[k + 1] if k + 1 < len(mine) else None
            if nxt is None or math.hypot(nxt[2] - x, nxt[4] - z) > GAP_M:
                continue
        out.append((x, y, z))
        last = i
    return out


def ghost_path(data):
    return stream_path(ghost_stream(data))


def carlog_paths(text):
    """The player's car (kind 0) of a tools/carlog.py log: lines "t kind slot x z"."""
    pts = []
    for line in text.splitlines():
        f = line.split()
        if len(f) == 5 and f[1] == "0":
            try:
                pts.append((float(f[3]), 0.0, float(f[4])))
            except ValueError:
                pass
    return pts


# ---- geometry -------------------------------------------------------------------------------

def split_path(pts, gap=GAP_M):
    """Pieces of a path without jumps longer than gap (x, z only)."""
    pieces, cur = [], []
    for p in pts:
        q = (p[0], p[2]) if len(p) == 3 else p
        if cur and math.hypot(q[0] - cur[-1][0], q[1] - cur[-1][1]) > gap:
            pieces.append(cur)
            cur = []
        cur.append(q)
    if cur:
        pieces.append(cur)
    return [c for c in pieces if len(c) > 1]


def resample(pts, step=STEP_M):
    """Points every step metres along a polyline (the ends kept)."""
    if len(pts) < 2:
        return list(pts)
    out, carry = [pts[0]], 0.0
    for (ax, az), (bx, bz) in zip(pts, pts[1:]):
        seg = math.hypot(bx - ax, bz - az)
        d = step - carry
        while d <= seg:
            t = d / seg
            out.append((ax + (bx - ax) * t, az + (bz - az) * t))
            d += step
        carry = seg - (d - step)
    if math.hypot(out[-1][0] - pts[-1][0], out[-1][1] - pts[-1][1]) > step * 0.25:
        out.append(pts[-1])
    return out


def simplify(pts, eps=SIMPLIFY_M):
    """Douglas-Peucker (iterative)."""
    if len(pts) < 3:
        return list(pts)
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, az = pts[a]
        bx, bz = pts[b]
        dx, dz = bx - ax, bz - az
        n = math.hypot(dx, dz)
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            px, pz = pts[i]
            d = abs(dx * (az - pz) - dz * (ax - px)) / n if n else math.hypot(px - ax, pz - az)
            if d > best:
                best, bi = d, i
        if best > eps:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(pts, keep) if k]


class Grid:
    """A spatial hash of points for radius queries."""

    def __init__(self, cell):
        self.cell, self.cells = cell, {}

    def key(self, x, z):
        return int(math.floor(x / self.cell)), int(math.floor(z / self.cell))

    def add(self, x, z, item):
        self.cells.setdefault(self.key(x, z), []).append((x, z, item))

    def near(self, x, z, r):
        """The nearest item within r, or None."""
        cx, cz = self.key(x, z)
        n = int(math.ceil(r / self.cell))
        best, bd = None, r * r
        for i in range(cx - n, cx + n + 1):
            for j in range(cz - n, cz + n + 1):
                for px, pz, item in self.cells.get((i, j), ()):
                    d = (px - x) ** 2 + (pz - z) ** 2
                    if d <= bd:
                        best, bd = item, d
        return best


def build_roads(paths):
    """Merge paths into road stretches: [(points, uses)], uses = how many paths drove it.
    Longer paths first; a sample within MERGE_M of a drawn road is that road; the new runs of
    a path (with one sample of the road they leave and join) become stretches."""
    pieces = []
    for p in paths:
        for piece in split_path(p):
            r = resample(piece)
            if len(r) >= 2:
                pieces.append(r)
    pieces.sort(key=len, reverse=True)
    grid = Grid(MERGE_M)
    roads = []          # [samples]
    uses = []           # per road, per sample: set of piece indexes
    for pi, pts in enumerate(pieces):
        hit = [grid.near(x, z, MERGE_M) for x, z in pts]
        runs, start = [], None
        for i, h in enumerate(hit + [0]):
            if h is None and start is None:
                start = i
            elif h is not None and start is not None:
                runs.append((start, i))
                start = None
        new = []
        for a, b in runs:
            joined = (a > 0) + (b < len(pts))
            if b - a < MIN_RUN and joined < 2:
                continue
            seg = pts[max(0, a - 1):min(len(pts), b + 1)]
            new.append(seg)
        for h in hit:
            if h is not None:
                uses[h[0]][h[1]].add(pi)
        for seg in new:
            ri = len(roads)
            roads.append(seg)
            uses.append([{pi} for _ in seg])
            for si, (x, z) in enumerate(seg):
                grid.add(x, z, (ri, si))
    out = []
    for seg, u in zip(roads, uses):
        counts = sorted(len(s) for s in u)
        out.append((seg, counts[len(counts) // 2]))
    return out


# ---- the terrain ------------------------------------------------------------------------

class Raster:
    """A boolean / label raster over the map in cells of `cell` metres."""

    def __init__(self, bounds, cell):
        self.x0, self.z0, self.x1, self.z1 = bounds
        self.cell = cell
        self.w = int(math.ceil((self.x1 - self.x0) / cell))
        self.h = int(math.ceil((self.z1 - self.z0) / cell))

    def centre(self, i, j):
        return self.x0 + (i + 0.5) * self.cell, self.z0 + (j + 0.5) * self.cell

    def index(self, x, z):
        return int((x - self.x0) // self.cell), int((z - self.z0) // self.cell)


def distance_field(r, samples):
    """Chamfer distance (metres) from each cell to the nearest sample."""
    inf = float("inf")
    d = [[inf] * r.w for _ in range(r.h)]
    for x, z in samples:
        i, j = r.index(x, z)
        if 0 <= i < r.w and 0 <= j < r.h:
            d[j][i] = 0.0
    a, b = r.cell, r.cell * math.sqrt(2)
    for j in range(r.h):
        row = d[j]
        for i in range(r.w):
            v = row[i]
            if i > 0:
                v = min(v, row[i - 1] + a)
            if j > 0:
                up = d[j - 1]
                v = min(v, up[i] + a)
                if i > 0:
                    v = min(v, up[i - 1] + b)
                if i < r.w - 1:
                    v = min(v, up[i + 1] + b)
            row[i] = v
    for j in range(r.h - 1, -1, -1):
        row = d[j]
        for i in range(r.w - 1, -1, -1):
            v = row[i]
            if i < r.w - 1:
                v = min(v, row[i + 1] + a)
            if j < r.h - 1:
                dn = d[j + 1]
                v = min(v, dn[i] + a)
                if i > 0:
                    v = min(v, dn[i - 1] + b)
                if i < r.w - 1:
                    v = min(v, dn[i + 1] + b)
            row[i] = v
    return d


def outlines(r, mask):
    """Closed outlines (world points) of the filled cells of mask[j][i]: the cell edges with
    an empty neighbour, directed with the cell on the left (z up), chained into loops."""
    def filled(i, j):
        return 0 <= i < r.w and 0 <= j < r.h and mask[j][i]
    nxt = {}
    for j in range(r.h):
        for i in range(r.w):
            if not mask[j][i]:
                continue
            if not filled(i, j - 1):
                nxt.setdefault((i, j), []).append((i + 1, j))
            if not filled(i + 1, j):
                nxt.setdefault((i + 1, j), []).append((i + 1, j + 1))
            if not filled(i, j + 1):
                nxt.setdefault((i + 1, j + 1), []).append((i, j + 1))
            if not filled(i - 1, j):
                nxt.setdefault((i, j + 1), []).append((i, j))
    loops = []
    while nxt:
        start = next(iter(nxt))
        loop, cur = [start], start
        while True:
            ends = nxt.get(cur)
            if not ends:
                break
            v = ends.pop()
            if not ends:
                del nxt[cur]
            if v == start:
                break
            loop.append(v)
            cur = v
        if len(loop) > 3:
            loops.append([(r.x0 + i * r.cell, r.z0 + j * r.cell) for i, j in loop])
    return loops


def chaikin(pts, rounds=3, closed=True):
    for _ in range(rounds):
        out = []
        n = len(pts)
        rng = range(n) if closed else range(n - 1)
        for k in rng:
            (ax, az), (bx, bz) = pts[k], pts[(k + 1) % n]
            out.append((ax * 0.75 + bx * 0.25, az * 0.75 + bz * 0.25))
            out.append((ax * 0.25 + bx * 0.75, az * 0.25 + bz * 0.75))
        if not closed:
            out = [pts[0]] + out + [pts[-1]]
        pts = out
    return pts


def soften(loop, cell):
    """A cell outline made a soft shape: the stairs simplified away, then rounded."""
    pts = simplify(loop + loop[:1], cell * 1.1)[:-1]
    if len(pts) < 3:
        return loop
    return simplify(chaikin(pts, 4) + [chaikin(pts, 4)[0]], 6)[:-1]


def area(pts):
    return 0.5 * sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(pts, pts[1:] + pts[:1]))


def inside(pt, poly):
    x, z = pt
    c = False
    for (ax, az), (bx, bz) in zip(poly, poly[1:] + poly[:1]):
        if (az > z) != (bz > z) and x < (bx - ax) * (z - az) / (bz - az) + ax:
            c = not c
    return c


# ---- SVG ----------------------------------------------------------------------------------

class Svg:
    def __init__(self, bounds):
        self.x0, self.z0, self.x1, self.z1 = bounds
        self.w = int(round((self.x1 - self.x0) / UNIT))
        self.h = int(round((self.z1 - self.z0) / UNIT))

    def xy(self, x, z):
        return (x - self.x0) / UNIT, (self.z1 - z) / UNIT

    def path(self, pts, closed=False, prec=0):
        """An SVG path: absolute move, then relative lines with rounded deltas (the rounding
        error is carried, so the path does not drift)."""
        if not pts:
            return ""
        f = (lambda v: str(int(round(v)))) if prec == 0 else (lambda v: ("%.*f" % (prec, v)).rstrip("0").rstrip("."))
        sx, sy = self.xy(*pts[0])
        q = 10 ** prec
        cx, cy = round(sx * q) / q, round(sy * q) / q
        out = ["M%s %s" % (f(cx), f(cy))]
        deltas = []
        for p in pts[1:]:
            x, y = self.xy(*p)
            nx, ny = round(x * q) / q, round(y * q) / q
            if nx == cx and ny == cy:
                continue
            deltas.append("%s %s" % (f(nx - cx), f(ny - cy)))
            cx, cy = nx, ny
        if deltas:
            out.append("l" + " ".join(deltas))
        if closed:
            out.append("z")
        return "".join(out).replace(" -", "-")


ICONS = {
    # Our own icons in the logo's style: a rounded badge, flat facets, a cream outline.
    "festival": '<path d="M-9 6 L0-9 L9 6 Z" fill="var(--map-icon-a,#ff5470)"/>'
                '<path d="M0-9 L9 6 L0 6 Z" fill="var(--map-icon-b,#ff9d3c)"/>'
                '<path d="M-5-2 h10 M-7 2 h14" stroke="var(--map-icon-line,#ffe9c0)" stroke-width="1.6"/>'
                '<circle cy="-9" r="2" fill="var(--map-icon-line,#ffe9c0)"/>',
    "outpost": '<path d="M-7 7 V-1 L0-7 L7-1 V7 Z" fill="var(--map-icon-b,#ff9d3c)"/>'
               '<path d="M0-7 L7-1 V7 H0 Z" fill="var(--map-icon-a,#ff5470)"/>'
               '<rect x="-2.5" y="1" width="5" height="6" fill="var(--map-icon-ink,#241747)"/>',
    "dam": '<path d="M-9-6 H9 L6 7 H-6 Z" fill="var(--map-icon-b,#ff9d3c)"/>'
           '<path d="M0-6 H9 L6 7 H0 Z" fill="var(--map-icon-a,#ff5470)"/>'
           '<path d="M-5 0 h10 M-4 4 h8" stroke="var(--map-icon-line,#ffe9c0)" stroke-width="1.4"/>',
    "airfield": '<path d="M0-9 L2 -2 L9 1 V3 L2 1.5 L1.5 6 L4 8 V9 L0 8 L-4 9 V8 L-1.5 6 L-2 1.5 L-9 3 V1 L-2-2 Z" '
                'fill="var(--map-icon-b,#ff9d3c)"/>'
                '<path d="M0-9 L2-2 L9 1 V3 L2 1.5 L1.5 6 L4 8 V9 L0 8 Z" fill="var(--map-icon-a,#ff5470)"/>',
    "town": '<path d="M-8 7 V-1 L-4-5 L0-1 V7 Z" fill="var(--map-icon-b,#ff9d3c)"/>'
            '<path d="M0 7 V-5 L4-8 L8-5 V7 Z" fill="var(--map-icon-a,#ff5470)"/>'
            '<path d="M-5 2 h2 M3-2 h2 M3 2 h2" stroke="var(--map-icon-ink,#241747)" stroke-width="1.6"/>',
}
ICON_BADGE = '<rect x="-12" y="-12" width="24" height="24" rx="7" fill="var(--map-icon-bg,#241747)" ' \
             'stroke="var(--map-icon-line,#ffe9c0)" stroke-width="1.5"/>'

STYLE = """
.cm-land{fill:var(--map-land,#2a1c52)}
.cm-drop{fill:var(--map-drop,#0b0722);fill-opacity:.55}
.cm-shore{fill:none;stroke:var(--map-shore,#ffe9c0);stroke-opacity:.18;stroke-width:3}
.cm-plains{fill:var(--map-plains,#33245f)}
.cm-hills{fill:var(--map-hills,#3a2768)}
.cm-foothills{fill:var(--map-foothills,#402a6c)}
.cm-mountains{fill:var(--map-mountains,#47307a)}
.cm-redrock{fill:var(--map-redrock,#5a2a55)}
.cm-water{fill:var(--map-water,#1f3f78);stroke:var(--map-water-edge,#5fa8e8);stroke-opacity:.55;stroke-width:2}
.cm-peak-a{fill:var(--map-peak-a,#5a3f96)}.cm-peak-b{fill:var(--map-peak-b,#3a2770)}
.cm-snow{fill:var(--map-snow,#fff4e6)}
.cm-mesa-a{fill:var(--map-mesa-a,#a3455b)}.cm-mesa-b{fill:var(--map-mesa-b,#74304f)}
.cm-tree{fill:var(--map-tree,#2f6a5a)}
.cm-field{fill:var(--map-field,#3c2b68)}
.cm-furrow{fill:none;stroke:var(--map-furrow,#4a3678);stroke-width:5;stroke-linecap:round}
.cm-wave{fill:none;stroke:var(--map-water-edge,#5fa8e8);stroke-opacity:.6;stroke-width:5;stroke-linecap:round}
.cm-roads path{fill:none;stroke-linecap:round;stroke-linejoin:round}
.cm-casing{stroke:var(--map-road-casing,#140d2e);stroke-opacity:.7;stroke-width:var(--map-rw-casing,15)}
.cm-minor{stroke:var(--map-road,#ffc98a);stroke-width:var(--map-rw-minor,6)}
.cm-main{stroke:var(--map-road-main,#ffe9c0);stroke-width:var(--map-rw-main,8)}
.cm-label{font:italic 800 14px system-ui,sans-serif;letter-spacing:.2em;text-transform:uppercase;fill:var(--map-label,#ffe9c0);fill-opacity:.32;paint-order:stroke;stroke:var(--map-name-halo,#2a1c52);stroke-opacity:.2;stroke-width:3px}
.cm-name{font:700 12px system-ui,sans-serif;fill:var(--map-name,#ffe9c0);paint-order:stroke;stroke:var(--map-name-halo,#2a1c52);stroke-width:3.5px;stroke-linejoin:round}
.cm-mk{pointer-events:none}
"""


def peak(svg, x, z, size, rnd):
    """A low-poly peak like the logo's mountain: a lit and a shaded facet, a snow cap."""
    cx, cy = svg.xy(x, z)
    w = size / UNIT
    h = w * rnd.uniform(0.7, 1.0)
    tx = cx + w * rnd.uniform(-0.15, 0.15)
    l, r_, b, t = cx - w / 2, cx + w / 2, cy + h / 3, cy - h * 2 / 3
    s = h * 0.28
    f = lambda v: "%d" % round(v)
    lit = "M%s %sL%s %sL%s %sZ" % (f(l), f(b), f(tx), f(t), f(tx), f(b))
    dark = "M%s %sL%s %sL%s %sZ" % (f(tx), f(t), f(r_), f(b), f(tx), f(b))
    snow = "M%s %sL%s %sL%s %sL%s %sZ" % (f(tx), f(t), f(tx - s * 0.55), f(t + s), f(tx), f(t + s * 0.7),
                                          f(tx + s * 0.5), f(t + s * 0.95))
    return lit, dark, snow


def mesa(svg, x, z, size, rnd):
    cx, cy = svg.xy(x, z)
    w = size / UNIT
    h = w * rnd.uniform(0.32, 0.45)
    top = w * rnd.uniform(0.45, 0.6)
    f = lambda v: "%d" % round(v)
    l, r_, b, t = cx - w / 2, cx + w / 2, cy + h / 2, cy - h / 2
    tl, tr = cx - top / 2, cx + top / 2
    front = "M%s %sL%s %sL%s %sL%s %sZ" % (f(l), f(b), f(tl), f(t), f(tr), f(t), f(r_), f(b))
    side = "M%s %sL%s %sL%s %sZ" % (f(tr), f(t), f(r_), f(b), f(cx + w * 0.1), f(b))
    return front, side


def tree(svg, x, z, size):
    cx, cy = svg.xy(x, z)
    w = size / UNIT
    f = lambda v: "%d" % round(v)
    return "M%s %sL%s %sL%s %sZ" % (f(cx - w / 2), f(cy + w * 0.6), f(cx), f(cy - w * 0.8), f(cx + w / 2), f(cy + w * 0.6))


def field(svg, x, z, size, rnd):
    """A patch of farmland: a tilted rectangle and its furrows."""
    cx, cy = svg.xy(x, z)
    w, h = size / UNIT, size / UNIT * rnd.uniform(0.5, 0.75)
    a = math.radians(rnd.uniform(-25, 25))
    ca, sa = math.cos(a), math.sin(a)
    f = lambda v: "%d" % round(v)

    def pt(u, v):
        return f(cx + u * ca - v * sa), f(cy + u * sa + v * ca)
    patch = "M%s %sL%s %sL%s %sL%s %sZ" % (pt(-w / 2, -h / 2) + pt(w / 2, -h / 2) + pt(w / 2, h / 2) + pt(-w / 2, h / 2))
    rows = "".join("M%s %sL%s %s" % (pt(-w / 2 + w * 0.12, v) + pt(w / 2 - w * 0.12, v))
                   for v in (-h / 4, 0, h / 4))
    return patch, rows


def waves(svg, poly):
    """Two short ripples near the middle of a lake."""
    cx = sum(x for x, _ in poly) / len(poly)
    cz = sum(z for _, z in poly) / len(poly)
    out = []
    for dx, dz in ((-120, 150), (90, -160)):
        x, y = svg.xy(cx + dx, cz + dz)
        out.append("M%d %dq12-8 24 0t24 0" % (round(x - 24), round(y)))
    return "".join(out)


def build_svg(roads, points, terrain):
    """The complete map SVG (a string)."""
    # The land reaches around the water too.
    extra_land = [tuple(p) for w in terrain.get("water", []) for p in w["outline"]]
    # Bounds: everything with a margin, in whole kilometres.
    xs = [p["x"] for p in points] + [x for seg, _ in roads for x, _ in seg] + [x for x, _ in extra_land]
    zs = [p["z"] for p in points] + [z for seg, _ in roads for _, z in seg] + [z for _, z in extra_land]
    margin = terrain.get("margin", 900)
    bounds = (math.floor((min(xs) - margin) / 500) * 500, math.floor((min(zs) - margin) / 500) * 500,
              math.ceil((max(xs) + margin) / 500) * 500, math.ceil((max(zs) + margin) / 500) * 500)
    svg = Svg(bounds)
    rnd = random.Random(316)

    # The landmass: within reach of a road or a point, closed (dilated then eroded).
    cell = terrain.get("cell", 125)
    r = Raster(bounds, cell)
    samples = [pt for seg, _ in roads for pt in seg[::4]] + [(p["x"], p["z"]) for p in points
                                                             if p["kind"] not in ("region",)]
    samples += list(extra_land)
    reach = terrain.get("reach", 1400)
    erode = terrain.get("erode", 500)
    d = distance_field(r, samples)
    grown = [[d[j][i] < reach + erode for i in range(r.w)] for j in range(r.h)]
    # erode: distance from the outside of the grown mask
    outside = [r.centre(i, j) for j in range(r.h) for i in range(r.w) if not grown[j][i]]
    edge = [(r.x0 - cell, r.z0 + j * cell) for j in range(r.h)] + [(r.x1 + cell, r.z0 + j * cell) for j in range(r.h)] \
        + [(r.x0 + i * cell, r.z0 - cell) for i in range(r.w)] + [(r.x0 + i * cell, r.z1 + cell) for i in range(r.w)]
    d2 = distance_field(r, outside + edge)
    land = [[grown[j][i] and d2[j][i] > erode for i in range(r.w)] for j in range(r.h)]
    land_loops = [soften(l, cell) for l in outlines(r, land) if abs(area(l)) > 4e6]

    # Regions: each land cell takes the kind of the nearest anchor.
    anchors = [(a[0], a[1], reg["kind"]) for reg in terrain["regions"] for a in reg["anchors"]]
    kinds = sorted({k for _, _, k in anchors})
    lab = [[None] * r.w for _ in range(r.h)]
    for j in range(r.h):
        for i in range(r.w):
            if land[j][i]:
                x, z = r.centre(i, j)
                lab[j][i] = min(anchors, key=lambda a: (a[0] - x) ** 2 + (a[1] - z) ** 2)[2]
    region_paths = {}
    for k in kinds:
        mask = [[lab[j][i] == k for i in range(r.w)] for j in range(r.h)]
        region_paths[k] = [soften(l, cell) for l in outlines(r, mask) if abs(area(l)) > 1e6]

    # Water: our own outlines (terrain.json), smoothed.
    water = [chaikin([tuple(p) for p in w["outline"]], 4) for w in terrain.get("water", [])]

    # Decorations: peaks on the mountains, mesas on the red rock, trees on the foothills, away
    # from roads, points and water.
    road_grid = Grid(200)
    for seg, _ in roads:
        for x, z in seg[::3]:
            road_grid.add(x, z, 1)
    for p in points:
        road_grid.add(p["x"], p["z"], 1)
    deco = {"peak": [], "mesa": [], "tree": [], "field": []}
    spacing = {"mountains": 760, "redrock": 900, "foothills": 520, "hills": 1100, "plains": 1050}
    keep = {"mountains": 0.75, "redrock": 0.7, "foothills": 0.55, "hills": 0.5, "plains": 0.42}
    for k, sp in spacing.items():
        loops = region_paths.get(k, [])
        if not loops:
            continue
        lx = [x for l in loops for x, _ in l]
        lz = [z for l in loops for _, z in l]
        z = min(lz)
        row = 0
        while z < max(lz):
            x = min(lx) + (sp / 2 if row % 2 else 0)
            while x < max(lx):
                px, pz = x + rnd.uniform(-sp * 0.3, sp * 0.3), z + rnd.uniform(-sp * 0.3, sp * 0.3)
                if rnd.random() < keep[k] and any(inside((px, pz), l) for l in loops) \
                        and road_grid.near(px, pz, 300) is None \
                        and not any(inside((px, pz), w) for w in water) \
                        and any(inside((px, pz), l) for l in land_loops):
                    if k == "mountains":
                        deco["peak"].append(peak(svg, px, pz, rnd.uniform(300, 560), rnd))
                    elif k == "redrock":
                        deco["mesa"].append(mesa(svg, px, pz, rnd.uniform(320, 520), rnd))
                    elif k == "plains":
                        deco["field"].append(field(svg, px, pz, rnd.uniform(420, 640), rnd))
                    else:
                        deco["tree"].append(tree(svg, px, pz, rnd.uniform(110, 170)))
                x += sp
            z += sp * 0.8
            row += 1

    # Roads: minor first, main on top; one casing path under all.
    main, minor = [], []
    for seg, uses in roads:
        s = simplify(seg)
        (main if uses >= MAIN_USES else minor).append(svg.path(s))

    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" class="cmap" '
           'data-x0="%d" data-z1="%d" data-unit="%g" role="img" aria-labelledby="cm-title">'
           % (svg.w, svg.h, bounds[0], bounds[3], UNIT),
           '<title id="cm-title">Illustrated map of Colorado: roads drawn from players\' driven lines</title>',
           "<defs><style>%s</style>" % " ".join(STYLE.split()).replace("; ", ";")]
    for name, body in ICONS.items():
        out.append('<symbol id="cm-i-%s" viewBox="-12 -12 24 24">%s%s</symbol>' % (name, ICON_BADGE, body))
    out.append('<radialGradient id="cm-glow"><stop offset="0" style="stop-color:var(--map-glow,#ff9d3c);stop-opacity:.42"/>'
               '<stop offset="1" style="stop-color:var(--map-glow,#ff9d3c);stop-opacity:0"/></radialGradient>')
    out.append("</defs>")
    out.append('<g class="cm-terrain">')
    out.append('<path class="cm-drop" transform="translate(0 22)" d="%s"/>' % "".join(svg.path(l, True) for l in land_loops))
    out.append('<path class="cm-land" d="%s"/>' % "".join(svg.path(l, True) for l in land_loops))
    for k in ("plains", "hills", "foothills", "mountains", "redrock"):
        if region_paths.get(k):
            out.append('<path class="cm-%s" d="%s"/>' % (k, "".join(svg.path(l, True) for l in region_paths[k])))
    out.append('<path class="cm-shore" d="%s"/>' % "".join(svg.path(l, True) for l in land_loops))
    fest = next((p for p in points if p["kind"] == "festival"), None)
    if fest:
        x, y = svg.xy(fest["x"], fest["z"])
        out.append('<circle class="cm-glow" cx="%d" cy="%d" r="%d" fill="url(#cm-glow)"/>' % (x, y, 1100 / UNIT))
    if deco["field"]:
        out.append('<path class="cm-field" d="%s"/>' % "".join(f_[0] for f_ in deco["field"]))
        out.append('<path class="cm-furrow" d="%s"/>' % "".join(f_[1] for f_ in deco["field"]))
    if deco["tree"]:
        out.append('<path class="cm-tree" d="%s"/>' % "".join(deco["tree"]))
    if deco["mesa"]:
        out.append('<path class="cm-mesa-a" d="%s"/>' % "".join(m[0] for m in deco["mesa"]))
        out.append('<path class="cm-mesa-b" d="%s"/>' % "".join(m[1] for m in deco["mesa"]))
    if deco["peak"]:
        out.append('<path class="cm-peak-a" d="%s"/>' % "".join(p[0] for p in deco["peak"]))
        out.append('<path class="cm-peak-b" d="%s"/>' % "".join(p[1] for p in deco["peak"]))
        out.append('<path class="cm-snow" d="%s"/>' % "".join(p[2] for p in deco["peak"]))
    if water:
        out.append('<path class="cm-water" d="%s"/>' % "".join(svg.path(w, True) for w in water))
        out.append('<path class="cm-wave" d="%s"/>' % "".join(waves(svg, w) for w in water))
    out.append("</g>")
    out.append('<g class="cm-roads">')
    out.append('<path class="cm-casing" d="%s"/>' % "".join(minor + main))
    out.append('<path class="cm-minor" d="%s"/>' % "".join(minor))
    out.append('<path class="cm-main" d="%s"/>' % "".join(main))
    out.append("</g>")

    # Labels and landmarks: constant on screen; the page scales them with --cm-s (the SVG units
    # per CSS pixel), so here they carry a transform for the whole-map view of a 1000 px map.
    s0 = svg.w / 1000.0
    names = {p["id"]: p for p in points}
    out.append('<g class="cm-labels">')
    for lb in terrain.get("labels", []):
        p = names.get(lb["id"])
        if not p:
            continue
        x, y = svg.xy(lb.get("x", p["x"]), lb.get("z", p["z"]))
        out.append('<g class="cm-mk" style="--x:%d;--y:%d" transform="translate(%d %d) scale(%.2f)">'
                   '<text class="cm-label" text-anchor="middle">%s</text></g>'
                   % (x, y, x, y, s0, esc(p["name"])))
    out.append("</g>")
    out.append('<g class="cm-landmarks">')
    for p in points:
        if p["kind"] not in ICONS:
            continue
        x, y = svg.xy(p["x"], p["z"])
        size = 22 if p["kind"] in ("festival", "dam", "airfield") else 18
        label = "" if p["kind"] == "outpost" else \
            '<text class="cm-name" y="%d" text-anchor="middle">%s</text>' % (size // 2 + 13, esc(p["name"]))
        out.append('<g class="cm-mk cm-lm cm-lm-%s" data-id="%s" style="--x:%d;--y:%d" '
                   'transform="translate(%d %d) scale(%.2f)"><title>%s</title>'
                   '<use href="#cm-i-%s" x="%d" y="%d" width="%d" height="%d"/>%s</g>'
                   % (p["kind"], p["id"], x, y, x, y, s0, esc(p["name"]), p["kind"],
                      -size // 2, -size // 2, size, size, label))
    out.append("</g>")
    out.append("</svg>")
    return "\n".join(out) + "\n"


def esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def read_paths(ghost_dirs, carlogs):
    paths, bad = [], 0
    for folder in ghost_dirs:
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".bin"):
                continue
            try:
                with open(os.path.join(folder, name), "rb") as f:
                    p = ghost_path(f.read())
            except (OSError, ValueError):
                bad += 1
                continue
            if len(p) > 10:
                paths.append(p)
    for log in carlogs:
        with open(log) as f:
            p = carlog_paths(f.read())
        if len(p) > 10:
            paths.append(p)
    return paths, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghosts", action="append", default=[], help="a folder of ghost .bin files")
    ap.add_argument("--carlog", action="append", default=[], help="a tools/carlog.py log of our own drive")
    ap.add_argument("--points", required=True, help="site/data/map_points.json")
    ap.add_argument("--terrain", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "map_terrain.json"))
    ap.add_argument("--out", required=True, help="the map SVG (site/images/colorado-map.svg)")
    a = ap.parse_args(argv)
    paths, bad = read_paths(a.ghosts, a.carlog)
    if not paths:
        raise SystemExit("no paths")
    with open(a.points) as f:
        points = json.load(f)["points"]
    with open(a.terrain) as f:
        terrain = json.load(f)
    roads = build_roads(paths)
    doc = build_svg(roads, points, terrain)
    with open(a.out, "w") as f:
        f.write(doc)
    km = sum(math.hypot(b[0] - a_[0], b[1] - a_[1]) for seg, _ in roads for a_, b in zip(seg, seg[1:])) / 1000
    print("%s: %d paths (%d unreadable), %d road stretches, %.0f km of road, %d bytes"
          % (a.out, len(paths), bad, len(roads), km, len(doc.encode())))


if __name__ == "__main__":
    main()
