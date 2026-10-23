"""The public leaderboards JSON (forza_leaderboards.py): shape, no XUIDs, ASCII names, cache,
rate limit, CORS.

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

import json
import os
import tempfile
import unittest

import forza_leaderboards as lb
import forza_live_lsp as lsp
import forza_live_server as live
import forza_scores as scores

XUIDS = [0x0009000000000A00 + i for i in range(1, 6)]


def seed(xuid, tag, kind, board, score, car=797, cls=7, stt=False):
    scores.submit(xuid, tag, kind, {
        "gameScoreboardId": board, "isServerTimeTrial": stt, "score": score,
        "carId": car, "carClassId": cls, "carPerformanceIndex": 0.8,
        "usedSTM": False, "usedTCS": False, "usedABS": False, "usedAutoBrake": False,
        "usedAutoShifting": False, "usedClutch": False})


def get(path, method="GET"):
    status, ctype, body = lb.handle(method, path)
    return status, json.loads(body)


class LeaderboardsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        scores.configure(self.dir.name)
        lb.configure()
        self.now = 1_700_000_000.0
        lb._clock = lambda: self.now
        seed(XUIDS[0], "Fast One", 1, 230, 45.0, cls=7)        # 162 km/h
        seed(XUIDS[1], "Café ドラ", 1, 230, 40.0, car=1398, cls=4)
        seed(XUIDS[2], "ドライ", 1, 230, 30.0, cls=4)  # nothing ASCII left
        seed(XUIDS[3], "ZoneKing", 2, 101, 50.0)
        seed(XUIDS[0], "Fast One", 3, 199, 107.228, cls=7)
        seed(XUIDS[1], "Slow", 3, 199, 93.294, cls=4)
        seed(XUIDS[4], "Monthly", 3, 7, 80.0, stt=True)
        with open(os.path.join(self.dir.name, "leaderboard_names.json"), "w") as f:
            json.dump({"boards": {"speed-trap-230": "I-70 Run"}, "cars": {"1398": "Lambo"}}, f)

    def tearDown(self):
        lb._clock = __import__("time").time
        self.dir.cleanup()

    def test_other_paths_are_not_ours(self):
        self.assertIsNone(lb.handle("GET", "/shader-lists"))
        self.assertIsNone(lb.handle("GET", "/leaderboardsX"))

    def test_list(self):
        status, data = get("/leaderboards")
        self.assertEqual(status, 200)
        ids = [b["id"] for b in data["boards"]]
        self.assertEqual(ids, ["speed-trap-230", "speed-zone-101", "event-199", "time-trial-7"])
        trap = data["boards"][0]
        self.assertEqual((trap["name"], trap["unit"], trap["better"], trap["players"]),
                         ("I-70 Run", "km/h", "higher", 3))
        self.assertEqual(trap["classes"], [4, 7])
        self.assertEqual(data["boards"][1]["name"], "Speed zone 101")  # fallback label
        self.assertEqual(data["boards"][2]["better"], "lower")

    def test_board_rows(self):
        status, data = get("/leaderboards/speed-trap-230?limit=2")
        self.assertEqual(status, 200)
        self.assertEqual(len(data["rows"]), 2)
        r = data["rows"][0]
        self.assertEqual((r["rank"], r["gamertag"], r["unit"], r["display"]),
                         (1, "Fast One", "km/h", "162.0 km/h"))
        self.assertEqual((r["car"], r["carId"], r["class"], r["classId"]),
                         ("Car 797", 797, "R3", 7))
        self.assertRegex(r["date"], r"^20\d\d-\d\d-\d\d$")
        self.assertEqual(data["rows"][1]["car"], "Lambo")
        self.assertEqual(data["players"], 3)

    def test_times_are_best_lowest(self):
        _, data = get("/leaderboards/event-199")
        self.assertEqual([r["gamertag"] for r in data["rows"]], ["Slow", "Fast One"])
        self.assertEqual(data["rows"][0]["display"], "1:33.294")
        self.assertEqual(data["rows"][0]["unit"], "s")

    def test_class_filter(self):
        _, data = get("/leaderboards/speed-trap-230?class=B")
        self.assertEqual([r["classId"] for r in data["rows"]], [4, 4])
        self.assertEqual([r["rank"] for r in data["rows"]], [1, 2])
        self.assertEqual([r["overallRank"] for r in data["rows"]], [2, 3])
        self.assertEqual(get("/leaderboards/speed-trap-230?class=4")[1]["rows"], data["rows"])
        self.assertEqual(get("/leaderboards/speed-trap-230?class=zz")[0], 400)

    def test_bad_requests(self):
        self.assertEqual(get("/leaderboards/speed-trap-999")[0], 404)
        self.assertEqual(get("/leaderboards/bogus-1")[0], 404)
        self.assertEqual(get("/leaderboards/../etc")[0], 404)
        self.assertEqual(get("/leaderboards/speed-trap-230?limit=0")[0], 400)
        self.assertEqual(get("/leaderboards/speed-trap-230?limit=101")[0], 400)
        self.assertEqual(get("/leaderboards/speed-trap-230?limit=x")[0], 400)
        self.assertEqual(get("/leaderboards", "POST")[0], 405)

    def test_numeric_inputs_reject_non_decimal_and_oversized_values(self):
        for value in ("\u00b2", "9" * 5000):
            with self.subTest(value=value[:20]):
                self.assertEqual(get("/leaderboards/event-" + value)[0], 404)
                self.assertEqual(get("/leaderboards/event-199?limit=" + value)[0], 400)

    def test_no_xuids_and_ascii(self):
        texts = [lb.handle("GET", p)[2].decode() for p in (
            "/leaderboards", "/leaderboards/snapshot", "/leaderboards/speed-trap-230",
            "/leaderboards/event-199", "/leaderboards/time-trial-7")]
        for text in texts:
            text.encode("ascii")
            for x in XUIDS:
                self.assertNotIn("%016X" % x, text.upper())
                self.assertNotIn(str(x), text)
            self.assertNotIn("xuid", text.lower())
        tags = [r["gamertag"] for r in get("/leaderboards/speed-trap-230")[1]["rows"]]
        self.assertEqual(tags[1], "Cafe")
        self.assertRegex(tags[2], r"^Driver[0-9A-F]{4}$")

    def test_player_id_is_opaque_and_stable(self):
        a = get("/leaderboards/speed-trap-230")[1]["rows"][0]["player"]
        lb.configure()  # the salt is kept on disk, so ids survive a restart
        b = get("/leaderboards/speed-trap-230")[1]["rows"][0]["player"]
        self.assertEqual(a, b)
        self.assertRegex(a, r"^[0-9a-f]{12}$")
        self.assertNotIn("%X" % XUIDS[0], a.upper())
        ids = {r["player"] for r in get("/leaderboards/speed-trap-230")[1]["rows"]}
        self.assertEqual(len(ids), 3)

    def test_snapshot(self):
        status, data = get("/leaderboards/snapshot")
        self.assertEqual(status, 200)
        self.assertEqual(len(data["boards"]), 4)
        self.assertEqual(len(data["boards"][0]["rows"]), 3)
        self.assertIn({"id": 6, "name": "S"}, data["classes"])

    def test_cache_and_expiry(self):
        _, first = get("/leaderboards/speed-trap-230")
        seed(XUIDS[3], "Newcomer", 1, 230, 100.0)
        self.assertEqual(get("/leaderboards/speed-trap-230")[1], first)  # still cached
        self.now += lb.CACHE_SECONDS + 1
        _, later = get("/leaderboards/speed-trap-230")
        self.assertEqual(later["rows"][0]["gamertag"], "Newcomer")

    def test_rate_limit(self):
        for i in range(lb.BUILDS_PER_MINUTE):
            self.assertEqual(lb.handle("GET", "/leaderboards/speed-trap-230?limit=%d" % (i % 100 + 1)
                                       if i < 100 else "/leaderboards/event-199?limit=%d" % (i - 99))[0], 200)
        self.assertEqual(lb.handle("GET", "/leaderboards/speed-zone-101")[0], 429)
        self.assertEqual(lb.handle("GET", "/leaderboards/speed-trap-230?limit=1")[0], 200)  # cached
        self.now += 61
        self.assertEqual(lb.handle("GET", "/leaderboards/speed-zone-101")[0], 200)

    def test_served_through_the_server_with_cors(self):
        status, ctype, body = lsp.handle(None, "GET", "/leaderboards", {}, b"", ("127.0.0.1", 1))
        self.assertEqual((status, ctype), (200, "application/json"))
        raw = live.response(status, body, ctype)
        self.assertIn(b"Access-Control-Allow-Origin: *", raw)
        self.assertIn(b"application/json", raw)


if __name__ == "__main__":
    unittest.main()
