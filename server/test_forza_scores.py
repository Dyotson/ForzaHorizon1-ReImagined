"""Scoreboard services in-process: the 1000 Club (ChallengeCompleted, its GetRows board).

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

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


if __name__ == "__main__":
    unittest.main()
