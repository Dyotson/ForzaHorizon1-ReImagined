"""tools/build_map_roads.py on a synthetic sat-nav file and ghosts (no game or player data in the repo):
python3 -m unittest discover -s tools -p 'test_build_map_roads.py'"""
import contextlib
import io
import math
import os
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build_map_roads as bmr  # noqa: E402


def deflate(data):
    c = zlib.compressobj(6, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def ghost(points, car=0x034A, junk=True):
    """A ghost as the game uploads it: the header, the car's setup block, then chunks of at
    most 4096 bytes of records (times, a stray 0x40 inside another record, car states)."""
    stream = bytearray(b"\0\0\0\0\x20\0\0\0")
    for i, (x, y, z) in enumerate(points):
        stream += b"\x19" + struct.pack(">f", i * 0.2)
        if junk and i % 7 == 3:
            # another record whose payload holds 0x40 and floats far from the line
            stream += b"\x44" + struct.pack(">H", car) + b"\x40" + struct.pack(">H", car) + struct.pack(">3f", 9000.0, 5.0, 9000.0)
        stream += b"\x40" + struct.pack(">H", car) + struct.pack(">3f", x, y, z) + bytes(19)
    setup = deflate(b"CAR SETUP" + bytes(200))
    body = b""
    for i in range(0, len(stream), 4096):
        chunk = bytes(stream[i:i + 4096])
        comp = deflate(chunk)
        body += struct.pack(">II", len(comp), len(chunk)) + comp
    head = struct.pack(">4I", len(setup) + 8, len(body), len(setup), 209)
    return head + setup + body


def line(x0, z0, x1, z1, n, y=100.0):
    return [(x0 + (x1 - x0) * i / (n - 1), y, z0 + (z1 - z0) * i / (n - 1)) for i in range(n)]


def navfile(segments):
    """A sat-nav file (the layout of tools/build_map_roads.py's docstring) of
    [(properties, [(x, z)])]: one node per point, one membership per node."""
    keys = sorted({k for pr, _ in segments for k in pr})
    vals = sorted({v for pr, _ in segments for v in pr.values()})
    nodes, order, segs, memb, props = b"", [], b"", b"", b""
    n = np = 0
    for si, (pr, pts) in enumerate(segments):
        segs += struct.pack(">4I", len(pts), len(order), len(pr), np)
        for k, (x, z) in enumerate(pts):
            nodes += struct.pack(">I3f4I", n, x, 12.5, z, 1, n, 0, 0)
            memb += struct.pack(">2I", si, k)
            order.append(n)
            n += 1
        for key, v in pr.items():
            props += struct.pack(">2I", keys.index(key), vals.index(v))
            np += 1
    def table(words):
        offs, blob = [], b""
        for w in words:
            offs.append(len(blob))
            blob += w.encode() + b"\0"
        return offs, blob
    koffs, kblob = table(keys)
    voffs, vblob = table(vals)
    head = struct.pack(">10I", 0x0E177551, n, len(segments), np, len(order), n, len(keys), len(vals), 0, 0)
    return (head + nodes + segs + struct.pack(">%dI" % len(order), *order) + memb + props
            + struct.pack(">%dI" % (len(koffs) + len(voffs)), *(koffs + voffs)) + kblob + vblob)


class NavTests(unittest.TestCase):
    def test_reads_the_roads_and_their_classes(self):
        data = navfile([
            ({"road_type": "freeway", "display_name": "interstate"}, [(0, 0), (100, 0), (200, 10)]),
            ({"road_type": "dirt"}, [(0, 50), (0, 150)]),
            ({"road_type": "shortcut"}, [(5, 5), (6, 6)]),                       # not drawn
            ({"area_type": "plains"}, [(0, 0), (900, 0), (900, 900), (0, 0)]),    # an area outline
            ({"display_name": "map:label", "road_type": "b"}, [(-50, -50), (-60, -70)]),
            ({"road_type": "a"}, [(1, 1)]),                                       # a single node
        ])
        roads = bmr.nav_roads(data)
        self.assertEqual(roads, [("freeway", [(0.0, 0.0), (100.0, 0.0), (200.0, 10.0)]),
                                 ("dirt", [(0.0, 50.0), (0.0, 150.0)]),
                                 ("minor", [(-50.0, -50.0), (-60.0, -70.0)])])
        with self.assertRaises(ValueError):
            bmr.nav_roads(data[:60])

    def test_chains_segments_at_their_shared_ends(self):
        a = ("main", [(0, 0), (100, 0)])
        b = ("main", [(200, 0), (100, 0)])         # reversed
        c = ("main", [(200, 0), (300, 50)])
        fork = [("main", [(400, 0), (500, 0)]), ("main", [(500, 0), (600, 50)]), ("main", [(500, 0), (600, -50)])]
        out = bmr.chain([a, b, c] + fork)
        lines = sorted((len(p), p[0], p[-1]) for _, p in out)
        self.assertEqual(len(out), 4)              # a-b-c joined; the fork stays three
        self.assertEqual(lines[-1][0], 4)
        self.assertEqual({lines[-1][1], lines[-1][2]}, {(0, 0), (300, 50)})


class GhostTests(unittest.TestCase):
    def test_decodes_the_car_positions(self):
        pts = line(1000, -200, 1600, 400, 300)  # 600 records: more than one chunk
        got = bmr.ghost_path(ghost(pts))
        self.assertEqual(len(got), len(pts))
        for (ax, ay, az), (bx, by, bz) in zip(got, pts):
            self.assertAlmostEqual(ax, bx, places=2)
            self.assertAlmostEqual(ay, by, places=2)
            self.assertAlmostEqual(az, bz, places=2)

    def test_rejects_what_is_not_a_ghost(self):
        good = ghost(line(0, 0, 100, 0, 20))
        for bad in (b"", b"\0" * 15, good[:-3], good[:4] + b"\0\0\0\1" + good[8:], b"PNG" + good[3:]):
            with self.assertRaises(ValueError):
                bmr.ghost_path(bad)

    def test_carlog(self):
        text = "1.0 0 0 10 20\n1.1 1 3 99 99\n1.2 0 0 12 22\nbad line\n"
        self.assertEqual(bmr.carlog_paths(text), [(10.0, 0.0, 20.0), (12.0, 0.0, 22.0)])


class GeometryTests(unittest.TestCase):
    def test_resample_spacing(self):
        r = bmr.resample([(0, 0), (100, 0), (100, 50)], 10)
        steps = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(r, r[1:])]
        self.assertEqual(r[0], (0, 0))
        self.assertEqual(r[-1], (100, 50))
        self.assertTrue(all(s <= 10.001 for s in steps))
        self.assertGreaterEqual(len(r), 15)

    def test_douglas_peucker(self):
        noisy = [(x, (1 if x % 2 else -1) * 0.5) for x in range(0, 101)]
        self.assertEqual(bmr.simplify(noisy, 2), [noisy[0], noisy[-1]])
        corner = [(x, 0) for x in range(0, 50)] + [(50, z) for z in range(0, 51)]
        s = bmr.simplify(corner, 1)
        self.assertEqual(s, [(0, 0), (50, 0), (50, 50)])

    def test_split_at_jumps(self):
        pts = [(0, 0, 0), (10, 0, 0), (500, 0, 0), (510, 0, 0)]
        self.assertEqual(len(bmr.split_path(pts)), 2)

    def test_paths_driven_twice_are_one_road(self):
        a = [(x, z) for x, _, z in line(0, 0, 2000, 0, 200)]
        b = [(x, z + 6) for x, _, z in line(0, 0, 2000, 0, 200)]         # the same road, 6 m over
        branch = [(x, z) for x, _, z in line(1000, 0, 1000, 800, 80)]    # a side road
        roads = bmr.build_roads([("driven", a), ("driven", b), ("driven", branch)])
        self.assertEqual(len(roads), 2)
        main = max(roads, key=lambda r: len(r[1]))[1]
        self.assertAlmostEqual(main[len(main) // 2][1], 3, delta=0.5)    # the middle of the two
        side = min(roads, key=lambda r: len(r[1]))[1]
        self.assertLess(math.hypot(side[0][0] - 1000, side[0][1]), bmr.MERGE_M + bmr.STEP_M)  # joins it

    def test_lanes_make_one_centreline_and_crossings_stay(self):
        lane1 = [(x, -15) for x in range(0, 3001, 50)]
        lane2 = [(x, 15) for x in range(3000, -1, -50)]                  # the other way
        cross = [(1500, z) for z in range(-800, 801, 50)]
        roads = bmr.build_roads([("freeway", lane1), ("freeway", lane2), ("minor", cross)])
        free = [p for c, p in roads if c == "freeway"]
        self.assertEqual(len(free), 1)
        mid = free[0][len(free[0]) // 2]
        self.assertAlmostEqual(mid[1], 0, delta=0.5)                       # between the lanes
        self.assertTrue(all(abs(z) < 0.5 for x, z in free[0] if 300 < x < 2700))  # not pulled by the crossing
        minor = [p for c, p in roads if c == "minor"]
        self.assertGreaterEqual(len(minor), 1)
        self.assertGreater(sum(len(p) for p in minor) * bmr.STEP_M, 1400)  # the crossing road is drawn

    def test_outlines_and_soft_shapes(self):
        r = bmr.Raster((0, 0, 1000, 1000), 100)
        mask = [[2 <= i <= 6 and 3 <= j <= 7 for i in range(r.w)] for j in range(r.h)]
        loops = bmr.outlines(r, mask)
        self.assertEqual(len(loops), 1)
        self.assertAlmostEqual(abs(bmr.area(loops[0])), 500 * 500)
        soft = bmr.soften(loops[0], 100)
        self.assertTrue(bmr.inside((450, 550), soft))
        self.assertFalse(bmr.inside((50, 50), soft))


class SvgTests(unittest.TestCase):
    def test_builds_the_map(self):
        with tempfile.TemporaryDirectory() as tmp:
            nav = os.path.join(tmp, "game", "media", "tracks", "colorado")
            os.makedirs(nav)
            with open(os.path.join(nav, "colorado.nav"), "wb") as f:
                f.write(navfile([
                    ({"road_type": "freeway"}, [(x, -1000) for x in range(-2000, 2001, 40)]),
                    ({"road_type": "freeway"}, [(x, -1030) for x in range(2000, -2001, -40)]),
                    ({"road_type": "a"}, [(0, z) for z in range(-1000, 2001, 40)]),
                    ({"road_type": "dirt"}, [(x, 1500) for x in range(0, 1501, 40)]),
                    ({"area_type": "plains"}, [(-3000, -3000), (3000, -3000), (3000, 3000)])]))
            os.mkdir(os.path.join(tmp, "ghosts"))
            with open(os.path.join(tmp, "ghosts", "0.bin"), "wb") as f:   # on the network: adds nothing
                f.write(ghost(line(-1500, -1000, 1500, -1000, 300)))
            with open(os.path.join(tmp, "ghosts", "1.bin"), "wb") as f:   # off it: a driven road
                f.write(ghost(line(-1500, 600, -500, 1200, 150)))
            with open(os.path.join(tmp, "ghosts", "bad.bin"), "wb") as f:
                f.write(b"not a ghost")
            points = os.path.join(tmp, "points.json")
            with open(points, "w") as f:
                f.write('{"points": [{"id": "speed-trap-201", "kind": "speed-trap", "name": "A Trap", "x": 0, "z": -1000},'
                        '{"id": "festival", "kind": "festival", "name": "Fest <&>", "x": -1500, "z": 500},'
                        '{"id": "region-x", "kind": "region", "name": "Hills", "x": 1500, "z": 1500}]}')
            terrain = os.path.join(tmp, "terrain.json")
            with open(terrain, "w") as f:
                f.write('{"regions": [{"kind": "plains", "anchors": [[-1000, 0]]}, {"kind": "mountains", "anchors": [[1500, 1500]]}],'
                        ' "water": [{"outline": [[1500, -2500], [2200, -2500], [2200, -2000], [1500, -2000]]}],'
                        ' "labels": [{"id": "region-x", "x": 1200, "z": 1300}]}')
            out = os.path.join(tmp, "map.svg")
            with contextlib.redirect_stdout(io.StringIO()) as said:
                bmr.main(["--game", os.path.join(tmp, "game"), "--ghosts", os.path.join(tmp, "ghosts"),
                          "--points", points, "--terrain", terrain, "--out", out])
            self.assertIn("6 lines (2 driven, 1 unreadable ghosts), 4 roads", said.getvalue())
            with open(out) as f:
                text = f.read()
        root = ET.fromstring(text)
        ns = "{http://www.w3.org/2000/svg}"
        self.assertEqual(root.tag, ns + "svg")
        paths = {e.get("class"): e.get("d") for e in root.iter(ns + "path")}
        for want in ("cm-land", "cm-plains", "cm-mountains", "cm-water", "cm-casing", "cm-freeway", "cm-main",
                     "cm-minor", "cm-dirt"):
            self.assertIn(want, paths)
        free = paths["cm-freeway"]
        self.assertEqual(free.count("M"), 1)    # the two carriageways are one road
        # north is up, and the freeway is the middle of its lanes (z -1015)
        z1, unit = int(root.get("data-z1")), float(root.get("data-unit"))
        self.assertAlmostEqual(float(free[1:].split("l")[0].replace("-", " -").split()[1]), (z1 + 1015) / unit, delta=1.5)
        self.assertEqual(paths["cm-minor"].count("M"), 1)   # the driven line off the network
        self.assertIn("Fest &lt;&amp;&gt;", text)
        self.assertIn('href="#cm-i-festival"', text)
        self.assertNotIn("A Trap", text)        # pins are the page's, not the map's
        self.assertIn(">Hills</text>", text)


if __name__ == "__main__":
    unittest.main()
