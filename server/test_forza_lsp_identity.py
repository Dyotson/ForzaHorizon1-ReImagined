"""Who a title service call is from (forza_live_lsp.verify_caller): the game names its player
in X-UserId, which anyone can write, and our game adds the friends key the XUID was bound to at
its HELLO (X-Friends-Key). Forged calls are refused when they change anything; calls without a
key (older games) are accepted but unverified. Drives the server's handlers directly."""

import json
import os
import struct
import tempfile
import unittest

import forza_live_lsp as lsp
import forza_live_server as srv
import forza_xtsw as x

ALL = srv.FEATURE_INVITE_PICK | srv.FEATURE_FRIENDS | srv.FEATURE_CHAT
KEY = bytes(15) + bytes([7])   # DriverA's friends key (as in its live_identity.txt)
XUID = 0x0009000000000001


class IdentityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.old_dir = lsp.DATA_DIR
        lsp.DATA_DIR = self.tmp.name
        lsp._unverified_logged.clear()
        self.server = srv.ForzaLive("test", 64)
        self.server.transport = type("T", (), {"sendto": lambda *_: None})()
        body = struct.pack(">HQQIH", srv.VERSION, 1, XUID, 0, 0)
        body += bytes([4]) + b"#TU4" + bytes([7]) + b"DriverA" + bytes([ALL]) + bytes([16]) + KEY
        self.server.datagram_received(srv.MAGIC + bytes([srv.HELLO]) + body, ("127.0.0.1", 20001))
        self.assertTrue(self.server.by_addr[("127.0.0.1", 20001)].friends_ok)

    def tearDown(self):
        lsp.DATA_DIR = self.old_dir
        self.tmp.cleanup()

    def call(self, cls, method, params_type, params, key=None, xuid=XUID):
        headers = {"x-classname": "Forza.WebServices.%s, Forza.WebServices" % cls,
                   "x-method": method, "x-userid": "0x%x" % xuid}
        if key is not None:
            headers["x-friends-key"] = key
        return lsp.service_call(headers, bytes(x.encode(params_type, params)), self.server)

    def race_stats(self, place, key=None, xuid=XUID):
        stats = {"finished": True, "userFinishPosition": place, "maxPlaces": 8, "hopperId": 3}
        return self.call("User", "WriteRaceStatsEx", x.UserWriteRaceStats_Head,
                         {"id": 1, "raceStats": stats}, key, xuid)

    def last_race(self, xuid=XUID):
        path = os.path.join(self.tmp.name, "players", "%016X.json" % xuid)
        if not os.path.exists(path):
            return None
        with open(path) as f:
            return json.load(f).get("lastRace")

    @staticmethod
    def status(reply):
        return struct.unpack_from(">II", reply)

    def test_the_player_s_own_key_is_trusted(self):
        self.assertEqual(self.status(self.race_stats(3, KEY.hex().upper())), (0, 0))
        race = self.last_race()
        self.assertEqual((race["userFinishPosition"], race["verified"]), (3, True))

    def test_another_key_cannot_write_for_the_player(self):
        self.race_stats(3, KEY.hex())
        forged = self.race_stats(1, ("AB" * 16))
        self.assertEqual(self.status(forged), (2, 5))
        self.assertEqual(self.last_race()["userFinishPosition"], 3)
        self.assertEqual(self.status(self.race_stats(1, "not hex")), (2, 5))
        self.assertEqual(self.status(self.race_stats(1, "00" * 15)), (2, 5))
        # scores and the profile are writes too; the next round's options change a world
        submit = self.call("Scoreboard", "SubmitScore", x.ScoreboardSubmitScore_Params,
                           {}, "CD" * 16)
        self.assertEqual(self.status(submit), (2, 5))
        options = self.call("Hopper", "GetGameOptions", x.HopperGetGameOptions_Params,
                            {"hopperId": 3, "roundNum": 1, "cchOptionXmlMax": 0x8000}, "CD" * 16)
        self.assertEqual(self.status(options), (2, 5))

    def test_reads_with_another_key_are_answered(self):
        reply = self.call("Hopper", "GetList", x.HopperGetList_Params,
                          {"maxCategoryResults": 2, "maxHopperResults": 32}, "EF" * 16)
        self.assertEqual(self.status(reply), (0, 0))

    def test_calls_without_a_key_are_accepted_unverified(self):
        with self.assertLogs("forza-live", "INFO") as logs:
            self.assertEqual(self.status(self.race_stats(2)), (0, 0))
            self.race_stats(2)
        self.assertEqual(self.last_race()["verified"], False)
        self.assertEqual(sum("accepted as unverified" in m for m in logs.output), 1)
        # an XUID that never signed in with a key cannot be checked either
        other = 0x0009000000000099
        self.assertEqual(self.status(self.race_stats(4, "AB" * 16, other)), (0, 0))
        self.assertEqual(self.last_race(other)["verified"], False)

    def test_caller_trusted_is_per_call(self):
        seen = []
        real = lsp.SERVICES[("User", "WriteRaceStatsEx")]
        lsp.SERVICES[("User", "WriteRaceStatsEx")] = lambda xuid, body: (seen.append(lsp.caller_trusted()), real(xuid, body))[1]
        try:
            self.race_stats(1, KEY.hex())
            self.race_stats(1)
        finally:
            lsp.SERVICES[("User", "WriteRaceStatsEx")] = real
        self.assertEqual(seen, [True, False])
        self.assertFalse(lsp.caller_trusted())


if __name__ == "__main__":
    unittest.main()
