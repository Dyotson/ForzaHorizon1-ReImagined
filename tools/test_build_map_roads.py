"""tools/build_map_roads.py on synthetic ghosts (no game or player data in the repo):
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
        a = [(x, 0, z) for x, _, z in line(0, 0, 2000, 0, 200)]
        b = [(x, 0, z + 5) for x, _, z in line(0, 0, 2000, 0, 200)]      # the same road, 5 m over
        branch = [(x, 0, z) for x, _, z in line(1000, 0, 1000, 800, 80)]  # a side road
        roads = bmr.build_roads([a, b, branch])
        self.assertEqual(len(roads), 2)
        main = max(roads, key=lambda r: len(r[0]))
        self.assertEqual(main[1], 2)                         # driven by both paths
        side = min(roads, key=lambda r: len(r[0]))
        self.assertEqual(side[1], 1)
        self.assertLess(math.hypot(side[0][0][0] - 1000, side[0][0][1]), bmr.MERGE_M + bmr.STEP_M)  # joins it

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
            os.mkdir(os.path.join(tmp, "ghosts"))
            east_west = [line(-2000, -1000 + i, 2000, -1000 + i, 400) for i in range(bmr.MAIN_USES)]
            for i, pts in enumerate(east_west + [line(0, -1000, 0, 2000, 300)]):
                with open(os.path.join(tmp, "ghosts", "%d.bin" % i), "wb") as f:
                    f.write(ghost(pts))
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
                bmr.main(["--ghosts", os.path.join(tmp, "ghosts"), "--points", points, "--terrain", terrain, "--out", out])
            self.assertIn("%d paths (1 unreadable), 2 road stretches" % (bmr.MAIN_USES + 1), said.getvalue())
            with open(out) as f:
                text = f.read()
        root = ET.fromstring(text)
        ns = "{http://www.w3.org/2000/svg}"
        self.assertEqual(root.tag, ns + "svg")
        cls = {e.get("class") for e in root.iter()}
        for want in ("cm-land", "cm-plains", "cm-mountains", "cm-water", "cm-casing", "cm-main", "cm-minor"):
            self.assertIn(want, cls)
        main = next(e for e in root.iter(ns + "path") if e.get("class") == "cm-main").get("d")
        minor = next(e for e in root.iter(ns + "path") if e.get("class") == "cm-minor").get("d")
        self.assertTrue(main.startswith("M"))   # the road driven MAIN_USES times
        self.assertTrue(minor.startswith("M"))
        # north is up: the east-west road (z -1000) is below the label at z 1300
        x0, z1, unit = int(root.get("data-x0")), int(root.get("data-z1")), float(root.get("data-unit"))
        y_road = (z1 - (-1000)) / unit
        self.assertAlmostEqual(float(main[1:].replace("l", " ").split()[1].split("l")[0]), y_road, delta=1)
        self.assertIn("Fest &lt;&amp;&gt;", text)
        self.assertIn('href="#cm-i-festival"', text)
        self.assertNotIn("A Trap", text)        # pins are the page's, not the map's
        self.assertIn(">Hills</text>", text)


if __name__ == "__main__":
    unittest.main()
