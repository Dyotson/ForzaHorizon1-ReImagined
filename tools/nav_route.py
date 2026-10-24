#!/usr/bin/env python3
"""nav_route.py NAV OUT X Z HX HZ [WX WZ ...]: a closed loop on the game's roads for tools/autodrive.py.

A test tool (#45: the hour-long free-roam soak in docs/perf.md). NAV is the sat-nav graph
(`media/tracks/colorado/colorado.nav`, format in tools/build_map_roads.py). The loop starts at
the road node nearest the car (X, Z: its position, HX HZ: its heading, as tools/autodrive.py's
car list reads them), goes through the road node nearest each waypoint WX WZ in turn and comes
back to the start in the direction it left. Each leg is a shortest path on the roads: oneway
segments only forward (a freeway's carriageways, one-way pairs), a, b and freeway roads at their
length, dirt roads, shortcuts and roads without traffic or AI at five times it, a road already
on the loop at eight times it, turns over 100 degrees at 1.5 km and over 150 degrees (U-turns) at
20 km. Two-way roads are driven 1.8 m right of the centreline.

Writes "x z speed" every ~4 m (AD_PATH_LOAD of tools/autodrive.py, without AD_OPEN: driven lap
after lap); the speed comes from the curvature (4.5 m/s^2 across, 3.5 m/s^2 braking ahead of a
bend, 8-30 m/s). Prints the length, the roads in order and the turns sharper than 110 degrees
left on the loop (where the car may need to back up).

Example (the save's spawn on County Route 203, heading east; Gladstone, Interstate 70, the town
on Route 29): nav_route.py extracted/game/media/tracks/colorado/colorado.nav loop.txt
87.7 -3921.2 0.98 -0.18 -174 -1058 -1879 -4657 -2300 -5100 (28.5 km, about 40 minutes a lap
behind the traffic).
"""
import heapq
import math
import struct
import sys

ROADS = ("a", "b", "freeway")
SLOW_ROADS = ("dirt", "shortcut")


def load(path):
    """(nodes [(x, z)], segments [(properties, [node index])]) of a sat-nav file."""
    data = open(path, "rb").read()
    h = struct.unpack(">10I", data[:40])
    n_nodes, n_segs, n_props, n_list, n_memb, n_keys, n_vals = h[1:8]
    p = 40
    nodes = [struct.unpack_from(">4xf4xf", data, p + 32 * i) for i in range(n_nodes)]
    p += 32 * n_nodes
    segs = [struct.unpack_from(">4I", data, p + 16 * i) for i in range(n_segs)]
    p += 16 * n_segs
    order = struct.unpack_from(">%dI" % n_list, data, p)
    p += 4 * n_list + 8 * n_memb
    props = [struct.unpack_from(">2I", data, p + 8 * i) for i in range(n_props)]
    p += 8 * n_props
    offs = struct.unpack_from(">%dI" % (n_keys + n_vals), data, p)
    p += 4 * (n_keys + n_vals)

    def text(base, off):
        end = data.index(b"\0", base + off)
        return data[base + off:end].decode("latin-1")
    keys = [text(p, o) for o in offs[:n_keys]]
    vbase = p + offs[n_keys - 1] + len(keys[-1]) + 1
    vals = [text(vbase, o) for o in offs[n_keys:]]
    out = []
    for count, first, pcount, pfirst in segs:
        pr = {keys[k]: vals[v] for k, v in props[pfirst:pfirst + pcount] if k < len(keys) and v < len(vals)}
        out.append((pr, [i for i in order[first:first + count] if i < n_nodes]))
    return nodes, out


def graph(nodes, segs):
    """node -> [(next node, cost, segment, oneway)]"""
    adj = {}
    for si, (pr, idx) in enumerate(segs):
        kind = pr.get("road_type")
        if kind not in ROADS + SLOW_ROADS:
            continue
        busy = kind in ROADS and pr.get("traffic_disabled") != "true" and pr.get("ai_disabled") != "true"
        oneway = pr.get("oneway") == "true"
        for a, b in zip(idx, idx[1:]):
            cost = math.dist(nodes[a], nodes[b]) * (1.0 if busy else 5.0)
            adj.setdefault(a, []).append((b, cost, si, oneway))
            if not oneway:
                adj.setdefault(b, []).append((a, cost, si, oneway))
    return adj


def turn(nodes, p, u, v):
    """The heading change in degrees from p->u to u->v."""
    a = (nodes[u][0] - nodes[p][0], nodes[u][1] - nodes[p][1])
    b = (nodes[v][0] - nodes[u][0], nodes[v][1] - nodes[u][1])
    la, lb = math.hypot(*a), math.hypot(*b)
    if la < 1e-6 or lb < 1e-6:
        return 0.0
    c = max(-1.0, min(1.0, (a[0] * b[0] + a[1] * b[1]) / (la * lb)))
    return math.degrees(math.acos(c))


def turn_cost(angle):
    return 20000.0 if angle > 150 else 1500.0 if angle > 100 else 0.0


def loop(nodes, adj, start, heading, waypoints):
    """[(node, segment)] of the loop: start, the waypoints, start again."""
    def nearest(x, z):
        return min(adj, key=lambda i: math.dist(nodes[i], (x, z)))
    s0 = nearest(*start)
    # The node behind the car: of the start node's neighbours, the one most opposite its heading.
    nb = {v for v, *_ in adj.get(s0, [])} | {u for u in adj for v, *_ in adj[u] if v == s0}
    p0 = min(nb, key=lambda v: ((nodes[v][0] - nodes[s0][0]) * heading[0] + (nodes[v][1] - nodes[s0][1]) * heading[1])
             / (math.dist(nodes[v], nodes[s0]) or 1))
    used = set()
    route = [(s0, None)]
    state = (s0, p0)
    targets = [nearest(*w) for w in waypoints] + [s0]
    for k, t in enumerate(targets):
        final = k == len(targets) - 1
        dist = {state: 0.0}
        prev = {}
        queue = [(0.0, state)]
        goal = None
        while queue:
            d, st = heapq.heappop(queue)
            if d > dist.get(st, 1e18):
                continue
            u, pu = st
            # Back at the start: only in the direction the loop left it.
            if u == t and st != state and (not final or len(route) < 2 or turn(nodes, pu, u, route[1][0]) <= 100):
                goal = st
                break
            for v, w, si, _ in adj.get(u, []):
                if v == pu and len(adj.get(u, [])) > 1:
                    continue
                nd = d + w * (8.0 if (min(u, v), max(u, v)) in used else 1.0) + turn_cost(turn(nodes, pu, u, v))
                ns = (v, u)
                if nd < dist.get(ns, 1e18):
                    dist[ns] = nd
                    prev[ns] = (st, si)
                    heapq.heappush(queue, (nd, ns))
        if goal is None:
            sys.exit("no road to waypoint %d" % k)
        legs = []
        st = goal
        while st != state:
            pst, si = prev[st]
            legs.append((st[0], si))
            st = pst
        legs.reverse()
        last = state[0]
        for v, _ in legs:
            used.add((min(last, v), max(last, v)))
            last = v
        route += legs
        state = goal
    return route[:-1]  # the last node is the first


def path(nodes, segs, route):
    """[(x, z)] every ~4 m, two-way stretches 1.8 m right of the centreline."""
    oneway = [segs[si if si is not None else route[1][1]][0].get("oneway") == "true" for _, si in route]
    pts = []
    for k, (n, _) in enumerate(route):
        x, z = nodes[n]
        a = nodes[route[k - 1][0]]
        b = nodes[route[(k + 1) % len(route)][0]]
        dx, dz = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dz) or 1
        if not oneway[k]:
            x += 1.8 * dz / length
            z -= 1.8 * dx / length
        pts.append((x, z))
    res = [pts[0]]
    for a, b in zip(pts, pts[1:] + pts[:1]):
        n = max(1, int(math.dist(a, b) / 4))
        for j in range(1, n + 1):
            p = (a[0] + (b[0] - a[0]) * j / n, a[1] + (b[1] - a[1]) * j / n)
            if math.dist(p, res[-1]) > 1.0:
                res.append(p)
    return res


def speeds(pts, vmax=30.0, lateral=4.5, brake=3.5):
    n = len(pts)
    sp = []
    for i in range(n):
        a, b, c = pts[(i - 3) % n], pts[i], pts[(i + 3) % n]
        h1 = math.atan2(b[1] - a[1], b[0] - a[0])
        h2 = math.atan2(c[1] - b[1], c[0] - b[0])
        dh = abs((h2 - h1 + math.pi) % (2 * math.pi) - math.pi)
        length = math.dist(a, b) + math.dist(b, c)
        curv = dh / length if length > 0 else 0
        sp.append(min(vmax, math.sqrt(lateral / curv)) if curv > 1e-6 else vmax)
    for _ in range(2):
        for i in range(n - 1, -1, -1):
            j = (i + 1) % n
            sp[i] = min(sp[i], math.sqrt(sp[j] ** 2 + 2 * brake * math.dist(pts[i], pts[j])))
    return [max(8.0, v) for v in sp]


def main():
    args = sys.argv[1:]
    if len(args) < 6 or len(args) % 2:
        sys.exit(__doc__)
    nodes, segs = load(args[0])
    adj = graph(nodes, segs)
    x, z, hx, hz = (float(a) for a in args[2:6])
    waypoints = [(float(args[i]), float(args[i + 1])) for i in range(6, len(args), 2)]
    route = loop(nodes, adj, (x, z), (hx, hz), waypoints)
    pts = path(nodes, segs, route)
    sp = speeds(pts)
    with open(args[1], "w") as f:
        f.writelines("%.2f %.2f %.2f\n" % (p[0], p[1], v) for p, v in zip(pts, sp))
    n = len(pts)
    length = sum(math.dist(pts[i], pts[(i + 1) % n]) for i in range(n))
    names = []
    for _, si in route[1:]:
        name = segs[si][0].get("display_name") or segs[si][0].get("road_type")
        if not names or names[-1] != name:
            names.append(name)
    print("%d points, %.0f m, %.0f s at the path's speeds" % (n, length, sum(
        math.dist(pts[i], pts[(i + 1) % n]) / sp[i] for i in range(n))))
    print("roads:", " > ".join(names))
    for i in range(n):
        a, b, c = pts[(i - 4) % n], pts[i], pts[(i + 4) % n]
        h1 = math.atan2(b[1] - a[1], b[0] - a[0])
        h2 = math.atan2(c[1] - b[1], c[0] - b[0])
        dh = math.degrees(abs((h2 - h1 + math.pi) % (2 * math.pi) - math.pi))
        if dh > 110:
            print("sharp turn %.0f degrees at point %d (%.0f, %.0f)" % (dh, i, b[0], b[1]))


if __name__ == "__main__":
    main()
