"""Players' shader lists (#276, forza_shader_lists.py): who may upload, the caps, validation of
every field, the distinct-player threshold, the seed list and persistence. No sockets."""

import json
import os
import sys
import tempfile
import unittest
from types import SimpleNamespace

import forza_friends as fr
import forza_shader_lists as sl

VERSION = "fbo:20260228/20261018"
TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")


def keyof(n):
    return bytes([n]) * 16


def pipeline(n, parts=("main",)):
    fields = [0] * len(sl.FIELD_VALUES)
    fields[1] = n % 32
    return {"vs": "%016X" % (0x1000 + n), "vs_mod": "0000000000000003", "ps": "%016X" % (0x2000 + n),
            "ps_mod": "0000000000000001", "fields": fields, "partitions": list(parts)}


def upload_body(pipelines, shaders=None, version=VERSION):
    return json.dumps({"version": version, "pipelines": pipelines,
                       "shaders": shaders if shaders is not None else {}}).encode()


class ListsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        sl.configure(self.tmp.name)
        self.store = fr.Store()
        self.server = SimpleNamespace(friends=SimpleNamespace(store=self.store))
        for n in range(1, 6):
            self.store.get(0x0009000000000000 + n, create=True)
            self.assertTrue(self.store.claim(0x0009000000000000 + n, keyof(n)))

    def tearDown(self):
        self.tmp.cleanup()

    def headers(self, n, key=None):
        return {"x-xuid": "%016X" % (0x0009000000000000 + n),
                "x-friends-key": (key or keyof(n)).hex()}

    def post(self, n, pipelines, shaders=None, **kw):
        return sl.handle(self.server, "POST", "/shader-lists/upload", self.headers(n, kw.get("key")),
                         kw.get("body") or upload_body(pipelines, shaders))

    def get(self, version=VERSION):
        status, _type, body = sl.handle(self.server, "GET", "/shader-lists?version=" + version.replace("/", "%2F"),
                                        {}, b"")
        return status, json.loads(body)

    def test_table_matches_the_tool(self):
        sys.path.insert(0, TOOLS)
        try:
            import fxobj_shaders
            import pipeline_list
        finally:
            sys.path.remove(TOOLS)
        self.assertEqual(tuple(c for _n, _w, c in pipeline_list.FIELDS), sl.FIELD_VALUES)
        self.assertEqual(fxobj_shaders.FETCH_LAYOUT_MASK, sl.FETCH_LAYOUT_MASK)
        self.assertEqual(fxobj_shaders.EXPORT_MASK, sl.EXPORT_MASK)
        self.assertEqual(sorted(pipeline_list.PARTITION_FLAGS), sorted(sl.PARTITIONS))
        self.assertEqual(pipeline_list.LIST_FORMAT, sl.LIST_FORMAT)

    def test_auth(self):
        status, _, _ = self.post(1, [pipeline(1)])
        self.assertEqual(status, 200)
        for headers in ({}, self.headers(1, keyof(9)), {"x-xuid": "zz", "x-friends-key": "00"},
                        {"x-xuid": "%016X" % 0x0009000000000099, "x-friends-key": keyof(1).hex()},
                        {"x-xuid": self.headers(1)["x-xuid"], "x-friends-key": "00"}):
            status, _, body = sl.handle(self.server, "POST", "/shader-lists/upload", headers,
                                        upload_body([pipeline(2)]))
            self.assertEqual(status, 403, headers)
        self.assertEqual(self.get()[1]["pipelines"], [])

    def test_unbound_xuid_refused(self):
        self.store.get(0x0009000000000077, create=True)  # seen, but no friends key bound yet
        h = {"x-xuid": "%016X" % 0x0009000000000077, "x-friends-key": keyof(7).hex()}
        status, _, _ = sl.handle(self.server, "POST", "/shader-lists/upload", h, upload_body([]))
        self.assertEqual(status, 403)

    def test_threshold_counts_distinct_players(self):
        sl_entry = pipeline(5)
        for _ in range(5):                      # one player, however often
            self.assertEqual(self.post(1, [sl_entry])[0], 200)
        self.assertEqual(self.get()[1]["pipelines"], [])
        self.post(2, [sl_entry])
        self.assertEqual(self.get()[1]["pipelines"], [])
        self.post(3, [sl_entry, pipeline(6)])
        listed = self.get()[1]["pipelines"]
        self.assertEqual(len(listed), 1)
        self.assertEqual(listed[0]["vs"], sl_entry["vs"])
        self.assertEqual(sl.MIN_PLAYERS, 3)

    def test_partitions_join_and_versions_apart(self):
        for n, part in ((1, "main"), (2, "scene"), (3, "ui")):
            self.post(n, [pipeline(1, (part,))])
        listed = self.get()[1]["pipelines"]
        self.assertEqual(listed[0]["partitions"], ["main", "scene", "ui"])
        self.assertEqual(self.get("fbo:20260101/20261018")[1]["pipelines"], [])

    def test_recipes_published_with_their_pipelines(self):
        rec = {"type": "vs", "from": "bound", "base": "00000000000000AA", "fetch": [[1, 0x100000, 3, 4]],
               "export": [[2, 1]], "nop": [3]}
        shaders = {"%016X" % 0x1001: rec, "%016X" % 0x2001: {"type": "ps", "from": "file"},
                   "%016X" % 0x9999: {"from": "missing", "why": "not used"}}
        for n in (1, 2, 3):
            self.post(n, [pipeline(1)], shaders)
        out = self.get()[1]
        self.assertEqual(out["shaders"], {"%016X" % 0x1001: rec,
                                          "%016X" % 0x2001: {"type": "ps", "from": "file"}})
        self.assertEqual(out["storage_versions"], [VERSION])
        self.assertEqual(out["format"], 2)

    def test_most_reported_recipe_wins(self):
        good = {"type": "ps", "from": "file"}
        bad = {"type": "ps", "from": "missing", "why": "x"}
        h = "%016X" % 0x2001
        self.post(1, [pipeline(1)], {h: bad})
        for n in (2, 3):
            self.post(n, [pipeline(1)], {h: good})
        self.assertEqual(self.get()[1]["shaders"][h], good)

    def test_validation(self):
        p = pipeline(1)
        bad = []
        bad.append(dict(p, vs="xyz"))
        bad.append(dict(p, vs="00000000000000aa"))
        bad.append(dict(p, extra=1))
        bad.append({k: v for k, v in p.items() if k != "ps"})
        for i, count in enumerate(sl.FIELD_VALUES):  # every field one past its range
            f = list(p["fields"])
            f[i] = count
            bad.append(dict(p, fields=f))
        bad.append(dict(p, fields=p["fields"][:-1]))
        bad.append(dict(p, fields=p["fields"] + [0]))
        bad.append(dict(p, fields=[True] + p["fields"][1:]))
        bad.append(dict(p, fields=[-1] + p["fields"][1:]))
        bad.append(dict(p, fields=["0"] + p["fields"][1:]))
        bad.append(dict(p, partitions=[]))
        bad.append(dict(p, partitions=["main", "gpu"]))
        bad.append("string")
        for entry in bad:
            self.assertEqual(self.post(1, [entry])[0], 400, entry)
        recipes = [{"from": "file", "blob": "AA"}, {"from": "bound", "base": "zz"},
                   {"from": "bound", "base": "0" * 16, "fetch": [[1, 2, 3, 0x80000000 | 0x40000000]]},
                   {"from": "bound", "base": "0" * 16, "fetch": [[1, 2]]},
                   {"from": "bound", "base": "0" * 16, "export": [[1, 0x80000000]]},
                   {"from": "bound", "base": "0" * 16, "nop": ["a"]},
                   {"from": "file", "base": "0" * 16}, {"from": "other"},
                   {"from": "missing", "why": "x" * 81}, [1]]
        for r in recipes:
            self.assertEqual(self.post(1, [p], {"%016X" % 1: r})[0], 400, r)
        self.assertEqual(self.post(1, [p], {"nothex": {"from": "file"}})[0], 400)
        for version in ("", "bad", "fbo:2026", "vulkan:20260228/20261018", None, 5):
            self.assertEqual(self.post(1, [p], body=upload_body([p], version=version))[0], 400, version)
        self.assertEqual(self.post(1, [p], body=b"not json")[0], 400)
        self.assertEqual(self.post(1, [p], body=b"[1]")[0], 400)
        self.assertEqual(self.post(1, [p], body=json.dumps({"version": VERSION, "pipelines": [],
                                                            "x": 1}).encode())[0], 400)
        self.assertEqual(self.get()[1]["pipelines"], [])  # nothing of it was kept

    def test_caps(self):
        many = [pipeline(n) for n in range(sl.MAX_ENTRIES + 1)]
        status, _, body = self.post(1, many)
        self.assertEqual(status, 400)
        self.assertEqual(self.post(1, many[:sl.MAX_ENTRIES])[0], 200)
        big = upload_body([pipeline(1)], {"%016X" % i: {"from": "file"} for i in range(sl.MAX_SHADERS + 1)})
        self.assertEqual(self.post(1, [], body=big)[0], 400)
        sl.MAX_BODY, old = 100, sl.MAX_BODY
        try:
            self.assertEqual(self.post(1, [pipeline(1)])[0], 413)
        finally:
            sl.MAX_BODY = old

    def test_daily_cap_per_player(self):
        old = sl.MAX_PER_DAY
        sl.MAX_PER_DAY = 3
        try:
            self.assertEqual(self.post(1, [pipeline(1), pipeline(2)])[0], 200)
            self.assertEqual(self.post(1, [pipeline(3), pipeline(4)])[0], 429)
            self.assertEqual(self.post(1, [pipeline(3)])[0], 200)
            self.assertEqual(self.post(1, [pipeline(5)])[0], 429)
            self.assertEqual(self.post(2, [pipeline(5)])[0], 200)  # another player is not limited
            sl.load()["daily"][sl.player_id(0x0009000000000001)][0] = "2000-01-01"  # a new day
            self.assertEqual(self.post(1, [pipeline(6)])[0], 200)
        finally:
            sl.MAX_PER_DAY = old

    def test_versions_and_pipelines_kept_are_capped(self):
        old = sl.MAX_VERSIONS, sl.MAX_PIPELINES
        sl.MAX_VERSIONS, sl.MAX_PIPELINES = 2, 2
        try:
            self.assertEqual(sl.handle(self.server, "POST", "/shader-lists/upload", self.headers(1),
                                       upload_body([pipeline(1)], version="fbo:20260101/20261018"))[0], 200)
            self.assertEqual(self.post(1, [pipeline(1)])[0], 200)
            self.assertEqual(sl.handle(self.server, "POST", "/shader-lists/upload", self.headers(1),
                                       upload_body([pipeline(1)], version="fbo:20260109/20261018"))[0], 400)
            for n in (1, 2, 3):
                self.post(n, [pipeline(1), pipeline(2), pipeline(3)])
            self.assertEqual(len(self.get()[1]["pipelines"]), 2)
        finally:
            sl.MAX_VERSIONS, sl.MAX_PIPELINES = old

    def shaders_kept(self):
        return sum(len(v["shaders"]) for v in sl.load()["versions"].values())

    def test_recipes_without_a_kept_pipeline_are_dropped(self):
        # #305 review: shader-only uploads must not grow the file or dodge the daily quota.
        junk = {"%016X" % (0x9000 + i): {"from": "file", "type": "vs"} for i in range(900)}
        for _ in range(3):
            self.assertEqual(self.post(1, [], junk)[0], 200)
        self.assertEqual(self.shaders_kept(), 0)
        p = pipeline(7)
        own = {p["vs"]: {"from": "file", "type": "vs"}, p["ps"]: {"from": "file", "type": "ps"}}
        self.assertEqual(self.post(1, [p], dict(junk, **own))[0], 200)
        self.assertEqual(self.shaders_kept(), 2)

    def test_full_pipeline_cap_keeps_no_new_recipes(self):
        old = sl.MAX_PIPELINES
        sl.MAX_PIPELINES = 1
        try:
            a, b = pipeline(1), pipeline(2)
            recipes = {h: {"from": "file"} for h in (a["vs"], a["ps"], b["vs"], b["ps"])}
            self.assertEqual(self.post(1, [a, b], recipes)[0], 200)
            kept = sl.load()["versions"][VERSION]["shaders"]
            self.assertEqual(set(kept), {a["vs"], a["ps"]})
        finally:
            sl.MAX_PIPELINES = old

    def test_shader_store_and_daily_recipes_are_capped(self):
        old = sl.MAX_SHADERS_KEPT, sl.MAX_SHADERS_PER_DAY
        sl.MAX_SHADERS_KEPT, sl.MAX_SHADERS_PER_DAY = 3, 4
        try:
            ps = [pipeline(n) for n in range(3)]
            recipes = {h: {"from": "file"} for p in ps for h in (p["vs"], p["ps"])}
            self.assertEqual(self.post(1, ps[:2], recipes)[0], 200)        # 4 recipes, 3 kept
            self.assertEqual(self.shaders_kept(), 3)
            self.assertEqual(self.post(1, ps[2:], recipes)[0], 429)        # 4 + 2 > 4 a day
        finally:
            sl.MAX_SHADERS_KEPT, sl.MAX_SHADERS_PER_DAY = old

    def test_malformed_recipe_entries_are_400_without_changes(self):
        p = pipeline(1)
        for bad in ({"fetch": [42]}, {"export": [42]}, {"fetch": [None]}, {"export": [{"a": 1}]},
                    {"fetch": [[1, [2], 3, 4]]}):
            with self.subTest(bad=bad):
                recipe = dict({"from": "bound", "base": "00000000000000AA"}, **bad)
                status, _t, _b = self.post(1, [p], {p["vs"]: recipe})
                self.assertEqual(status, 400)
        self.assertEqual(sl.load()["versions"], {})

    def test_persistence(self):
        for n in (1, 2, 3):
            self.post(n, [pipeline(1)], {"%016X" % 0x1001: {"type": "vs", "from": "file"}})
        before = self.get()[1]
        path = os.path.join(self.tmp.name, "shader_lists.json")
        self.assertTrue(os.path.isfile(path))
        self.assertFalse(os.path.exists(path + ".tmp"))
        with open(path) as f:
            text = f.read()
        self.assertNotIn("0009000000000001", text)  # XUIDs are not kept
        sl.configure(self.tmp.name)                 # a restart
        self.assertEqual(self.get()[1], before)
        self.post(4, [pipeline(2)])                 # and goes on counting
        with open(path) as f:
            self.assertEqual(len(json.load(f)["versions"][VERSION]["pipelines"]), 2)

    def test_corrupt_file_starts_empty(self):
        with open(os.path.join(self.tmp.name, "shader_lists.json"), "w") as f:
            f.write("{broken")
        sl.configure(self.tmp.name)
        self.assertEqual(self.get()[1]["pipelines"], [])
        self.assertEqual(self.post(1, [pipeline(1)])[0], 200)

    def test_seed_list(self):
        seed = {"format": 2, "title": "4D5309C9", "storage_versions": [VERSION],
                "pipelines": [pipeline(40), dict(pipeline(41), vs="bad")],
                "shaders": {"%016X" % 0x1028: {"type": "vs", "from": "file"}, "bad": {"from": "file"}}}
        with open(os.path.join(self.tmp.name, "shader_lists_seed.json"), "w") as f:
            json.dump(seed, f)
        out = self.get()[1]
        self.assertEqual([p["vs"] for p in out["pipelines"]], ["%016X" % 0x1028])
        self.assertEqual(list(out["shaders"]), ["%016X" % 0x1028])
        self.assertEqual(self.get("fbo:20260101/20261018")[1]["pipelines"], [])

    def test_routes(self):
        self.assertIsNone(sl.handle(self.server, "GET", "/other", {}, b""))
        self.assertEqual(sl.handle(self.server, "GET", "/shader-lists", {}, b"")[0], 400)
        self.assertEqual(sl.handle(self.server, "GET", "/shader-lists/upload", {}, b"")[0], 405)
        self.assertEqual(sl.handle(self.server, "POST", "/shader-lists", {}, b"")[0], 405)


if __name__ == "__main__":
    unittest.main()
