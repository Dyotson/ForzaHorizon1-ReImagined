"""Scoreboard services in-process: speed traps and zones, event times, the 1000 Club
(ChallengeCompleted, its GetRows board).

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

import json
import os
import struct
import tempfile
import unittest

import forza_scores as scores
import forza_xtsw as x
import forza_xtsw_all as A

A_XUID, B_XUID, C_XUID = 0x0009000000000A01, 0x0009000000000B02, 0x0009000000000C03
TAGS = {A_XUID: "DriverA", B_XUID: "DriverB", C_XUID: "DriverC"}
AVENTADOR, F40 = 1398, 1029


def results(reply, t):
    """The results of a success reply (12-byte header, status 0)."""
    assert struct.unpack(">I", reply[:4])[0] == 0, reply
    return x.decode(t, reply, 12)[0] if t is not None else None


class Club1000Test(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        scores.configure(self.dir.name)
        self.svc = scores.services(lambda xuid: TAGS.get(xuid, "%016X" % xuid))

    def tearDown(self):
        self.dir.cleanup()

    def complete(self, xuid, car, thumb):
        body = x.encode(A.ForzaChallengeChallengeCompleted_Params,
                        {"carId": car, "challengeThumbprint": thumb})
        return results(self.svc[("Challenge", "ChallengeCompleted")](xuid, bytes(body)), None)

    def rows(self, xuid, view=2, board=1, kind=scores.CLUB_TYPE, start=1, count=50):
        body = x.encode(x.ScoreboardGetRows_Params, {
            "type": kind, "view": view, "gameScoreboardId": board, "isServerTimeTrial": False,
            "isClubBoard": False, "startAt": start, "maxResults": count})
        return results(self.svc[("Scoreboard", "GetRows")](xuid, bytes(body)),
                       x.ScoreboardGetRows_Results)

    def test_empty_board(self):
        r = self.rows(A_XUID)
        self.assertEqual((r["rows"], r["available"], r["userRow"]["position"]), ([], 0, 0))

    def test_ranked_by_medals(self):
        self.complete(A_XUID, AVENTADOR, 0xAE6828D3)   # the game's thumbprint of ONE EIGHTY
        self.complete(B_XUID, AVENTADOR, 0xAE6828D3)
        self.complete(B_XUID, F40, 0x12345678)
        r = self.rows(A_XUID)
        got = [(row["position"], row["user"]["wzGamerTag"], row["score"], row["carId"])
               for row in r["rows"]]
        self.assertEqual(got, [(1, "DriverB", 2.0, F40), (2, "DriverA", 1.0, AVENTADOR)])
        self.assertEqual((r["userRow"]["position"], r["userRow"]["score"]), (2, 1.0))
        self.assertEqual((r["available"], r["totalRowCount"]), (2, 2))
        # the home screen's fetch (view 5, 200 rows) and a player without medals
        r = self.rows(C_XUID, view=5, count=200)
        self.assertEqual(len(r["rows"]), 2)
        self.assertEqual(r["userRow"]["position"], 0)

    def test_same_challenge_counts_once(self):
        for _ in range(3):
            self.complete(A_XUID, AVENTADOR, 0xAE6828D3)
        self.assertEqual(self.rows(A_XUID)["userRow"]["score"], 1.0)

    def test_tie_goes_to_the_first(self):
        self.complete(B_XUID, F40, 1)
        self.complete(A_XUID, F40, 1)
        scores.STORE.club["%016X" % A_XUID]["submitted"] += 10
        r = self.rows(A_XUID)
        self.assertEqual([row["user"]["wzGamerTag"] for row in r["rows"]], ["DriverB", "DriverA"])

    def test_kept_across_restarts(self):
        self.complete(A_XUID, AVENTADOR, 0xAE6828D3)
        scores.configure(self.dir.name)  # a new server process reads the file
        self.assertEqual(self.rows(B_XUID)["rows"][0]["user"]["wzGamerTag"], "DriverA")

    def test_limits(self):
        for t in range(scores.CLUB_MAX_PER_CAR + 5):
            self.complete(A_XUID, AVENTADOR, t)
        self.complete(A_XUID, 0, 7)  # no car
        self.assertEqual(self.rows(A_XUID)["userRow"]["score"], float(scores.CLUB_MAX_PER_CAR))

    def test_other_boards_untouched(self):
        self.complete(A_XUID, AVENTADOR, 0xAE6828D3)
        self.assertEqual(self.rows(A_XUID, kind=3, board=199)["available"], 0)
        self.assertEqual(self.rows(A_XUID, kind=4)["available"], 0)  # online XP

    def test_bad_parameters(self):
        with self.assertRaises(ValueError):
            self.svc[("Challenge", "ChallengeCompleted")](A_XUID, b"\x05")


class BoardsTest(unittest.TestCase):
    """Speed traps (type 1), speed zones (2) and event times (3): one board per type and id
    (the ids overlap), speeds better when bigger."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        scores.configure(self.dir.name)
        self.svc = scores.services(lambda xuid: TAGS.get(xuid, "%016X" % xuid))

    def tearDown(self):
        self.dir.cleanup()

    def submit(self, xuid, kind, board, score, ex=False):
        data = {"gameScoreboardId": board, "isServerTimeTrial": False, "score": score,
                "carId": AVENTADOR, "carClassId": 7, "carPerformanceIndex": 0.864}
        if ex:
            body = x.encode(x.ScoreboardSubmitScoreEx_Params,
                            {"type": kind, "data": {"base": data, "numCarsPassed": 0}})
            method = "SubmitScoreEx"
        else:
            body = x.encode(x.ScoreboardSubmitScore_Params, {"type": kind, "data": data})
            method = "SubmitScore"
        return results(self.svc[("Scoreboard", method)](xuid, bytes(body)),
                       x.ScoreboardSubmitScore_Results)

    def rows(self, xuid, kind, board, view=5, club=False):
        body = x.encode(x.ScoreboardGetRows_Params, {
            "type": kind, "view": view, "gameScoreboardId": board, "isServerTimeTrial": False,
            "isClubBoard": club, "startAt": 1, "maxResults": 200})
        r = results(self.svc[("Scoreboard", "GetRows")](xuid, bytes(body)),
                    x.ScoreboardGetRows_Results)
        return [(row["user"]["wzGamerTag"], round(row["score"], 3)) for row in r["rows"]], r

    def test_speed_trap_keeps_the_fastest(self):
        # The I-70 speed trap (board 230), as the game sent it: 45.088 m/s, no ghost.
        self.assertFalse(self.submit(A_XUID, scores.SPEED_TRAP, 230, 45.088)["uploadFiles"])
        self.submit(A_XUID, scores.SPEED_TRAP, 230, 40.0)   # slower: not kept
        self.submit(B_XUID, scores.SPEED_TRAP, 230, 52.5)
        self.submit(C_XUID, scores.SPEED_TRAP, 230, 30.25)
        got, r = self.rows(A_XUID, scores.SPEED_TRAP, 230)
        self.assertEqual(got, [("DriverB", 52.5), ("DriverA", 45.088), ("DriverC", 30.25)])
        self.assertEqual((r["userRow"]["position"], round(r["userRow"]["score"], 3)), (2, 45.088))
        self.submit(A_XUID, scores.SPEED_TRAP, 230, 60.0)   # faster: kept
        self.assertEqual(self.rows(C_XUID, scores.SPEED_TRAP, 230)[0][0], ("DriverA", 60.0))

    def test_types_do_not_share_a_board(self):
        # Event 230's time and the speed trap on board 230; a speed zone on board 101 and
        # event 101.
        self.assertTrue(self.submit(A_XUID, scores.EVENT_TIME, 230, 95.5, ex=True)["uploadFiles"])
        self.submit(A_XUID, scores.SPEED_TRAP, 230, 45.0)
        self.submit(B_XUID, scores.SPEED_ZONE, 101, 38.0)
        self.assertEqual(self.rows(B_XUID, scores.EVENT_TIME, 230)[0], [("DriverA", 95.5)])
        self.assertEqual(self.rows(B_XUID, scores.SPEED_TRAP, 230)[0], [("DriverA", 45.0)])
        self.assertEqual(self.rows(B_XUID, scores.SPEED_ZONE, 101)[0], [("DriverB", 38.0)])
        self.assertEqual(self.rows(B_XUID, scores.EVENT_TIME, 101)[0], [])

    def test_event_times_keep_the_fastest(self):
        self.submit(A_XUID, scores.EVENT_TIME, 199, 115.13, ex=True)
        self.assertFalse(self.submit(A_XUID, scores.EVENT_TIME, 199, 120.0, ex=True)["uploadFiles"])
        self.submit(B_XUID, scores.EVENT_TIME, 199, 107.228, ex=True)
        self.assertEqual(self.rows(A_XUID, scores.EVENT_TIME, 199)[0],
                         [("DriverB", 107.228), ("DriverA", 115.13)])

    def test_online_race_and_unknown_types_not_kept(self):
        self.submit(A_XUID, scores.ONLINE_XP_TYPE, -1, 1100.0)
        self.submit(A_XUID, 9, 5, 10.0)
        self.submit(A_XUID, scores.SPEED_TRAP, 201, float("nan"))
        self.assertEqual(scores.STORE.boards, {})

    def test_kept_across_restarts(self):
        self.submit(A_XUID, scores.SPEED_TRAP, 230, 45.088)
        scores.configure(self.dir.name)
        self.assertEqual(self.rows(B_XUID, scores.SPEED_TRAP, 230)[0], [("DriverA", 45.088)])

    def test_old_store_migrated(self):
        # Before: "sbid:stt" boards, the type in each entry, a speed kept as the lowest.
        old = {"boards": {"230:0": {
            "%016X" % A_XUID: {"xuid": "%016X" % A_XUID, "gamertag": "DriverA", "type": 1,
                               "score": 41.0, "submitted": 1, "scoreId": "00", "carId": 1,
                               "carClassId": 1, "carPerformanceIndex": 0.5, "ghostFileId": -1,
                               "usedSTM": 0, "usedTCS": 0, "usedABS": 0, "usedAutoBrake": 0,
                               "usedAutoShifting": 0, "usedClutch": 0},
            "%016X" % B_XUID: {"xuid": "%016X" % B_XUID, "gamertag": "DriverB", "type": 3,
                               "score": 99.0, "submitted": 2, "scoreId": "01", "carId": 1,
                               "carClassId": 1, "carPerformanceIndex": 0.5, "ghostFileId": -1,
                               "usedSTM": 0, "usedTCS": 0, "usedABS": 0, "usedAutoBrake": 0,
                               "usedAutoShifting": 0, "usedClutch": 0}}}}
        with open(os.path.join(self.dir.name, "scoreboards.json"), "w") as f:
            json.dump(old, f)
        scores.configure(self.dir.name)
        self.assertEqual(self.rows(C_XUID, scores.SPEED_TRAP, 230)[0], [("DriverA", 41.0)])
        self.assertEqual(self.rows(C_XUID, scores.EVENT_TIME, 230)[0], [("DriverB", 99.0)])
        self.submit(A_XUID, scores.SPEED_TRAP, 230, 44.0)
        self.assertEqual(self.rows(C_XUID, scores.SPEED_TRAP, 230)[0], [("DriverA", 44.0)])

    def test_board_limit(self):
        old = scores.MAX_BOARDS
        scores.MAX_BOARDS = 3
        try:
            for board in range(201, 206):
                self.submit(A_XUID, scores.SPEED_TRAP, board, 40.0)
            self.assertEqual(len(scores.STORE.boards), 3)
            self.assertEqual(self.rows(A_XUID, scores.SPEED_TRAP, 205)[0], [])
        finally:
            scores.MAX_BOARDS = old

    def test_club_board(self):
        import forza_clubs
        self.submit(A_XUID, scores.SPEED_TRAP, 230, 45.0)
        self.submit(B_XUID, scores.SPEED_TRAP, 230, 50.0)
        self.assertEqual(self.rows(A_XUID, scores.SPEED_TRAP, 230, club=True)[0], [])
        club = {"id": 7, "tag": "HZN"}
        orig = forza_clubs.membership
        forza_clubs.membership = lambda xuid: (club, 1) if xuid == A_XUID else (None, 0)
        try:
            self.assertEqual(self.rows(A_XUID, scores.SPEED_TRAP, 230, club=True)[0],
                             [("DriverA", 45.0)])
        finally:
            forza_clubs.membership = orig


if __name__ == "__main__":
    unittest.main()
