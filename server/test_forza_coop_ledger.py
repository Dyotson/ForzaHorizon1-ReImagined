"""The co-op campaign's ledger (#309 phase B, forza_coop_ledger.py): a campaign round recorded
when it starts, the players' places from their own reports cross-checked with the host's
XSessionWriteStats, only the requester's result offered to its launcher (signed like the
shader list upload), confirmations, the file across a restart. Drives the server's handlers
directly (no sockets), with the small game database of test_forza_campaign."""

import json
import os
import struct
import unittest

import forza_coop_ledger as ledger
import forza_live_lsp as lsp
import forza_stats as stats
import forza_world as world
from test_forza_campaign import CoopWorldCase


class LedgerTest(CoopWorldCase):
    def setUp(self):
        super().setUp()
        self.ledger_file = os.path.join(self.tmp.name, "coop_ledger.json")
        ledger.LEDGER = ledger.Ledger(self.ledger_file)
        stats.on_skill_write = ledger.LEDGER.host_score
        stats.STORE.pending.clear()
        stats.STORE.rated.clear()
        self.sid = self.coop_world()

    def tearDown(self):
        stats.on_skill_write = None
        stats.STORE.pending.clear()
        stats.STORE.rated.clear()
        ledger.LEDGER = ledger.Ledger()
        super().tearDown()

    def run_round(self, code="FR05"):
        """A starts the event, the world's members are known, the round starts."""
        self.say(self.a, "/event " + code)
        self.server.world_tick()
        self.game_options(self.a, lsp.COOP_HOPPER)  # the event is the next round
        self.game_options(self.a, lsp.COOP_HOPPER)  # its own prefetch: it started
        return next(iter(ledger.LEDGER.rounds))

    def host_writes(self, places, session=None):
        """The host's XSessionWriteStats at the race's end: cars - place for each human."""
        session = int(self.sid.hex(), 16) if session is None else session
        for peer, place in places.items():
            body = struct.pack(">BQQB", 1, session, peer.xuid, 1) + struct.pack(">IB", 0xFFFF0000, 1)
            body += struct.pack(">IBi", stats.RELATIVE_SCORE, stats.TYPE_INT32, 8 - place) + bytes(4)
            self.server.on_stats(self.a, body)

    def request(self, peer, method, path, body=b"", key=None):
        key = key or (bytes(15) + bytes([peer.id - self.a.id + 1])).hex()
        headers = {"x-xuid": "%016X" % peer.xuid, "x-friends-key": key}
        status, _, data = lsp.handle(self.server, method, path, headers, body, ("127.0.0.1", 1))
        return status, json.loads(data)

    def test_the_host_s_result_goes_to_its_launcher_once(self):
        rid = self.run_round()
        r = ledger.LEDGER.rounds[rid]
        self.assertEqual((r["event"], r["code"], r["by"], sorted(r["members"]), r["cars"]),
                         (4, "FR05", "%016X" % self.a.xuid,
                          sorted("%016X" % p.xuid for p in (self.a, self.b)), 8))
        self.host_writes({self.a: 3, self.b: 5})
        world.WORLDS.result(self.a.xuid, 3, 8, 1)
        world.WORLDS.result(self.b.xuid, 5, 8, 1)
        status, got = self.request(self.a, "GET", "/coop/results")
        self.assertEqual(status, 200)
        self.assertEqual([(e["id"], e["event"], e["code"], e["place"], e["places"])
                          for e in got["results"]], [(rid, 4, "FR05", 3, 8)])
        # milestone 1: only the requester's career
        self.assertEqual(self.request(self.b, "GET", "/coop/results")[1], {"results": []})
        # a confirmation by someone else changes nothing; the player's own removes it
        self.assertEqual(self.request(self.b, "POST", "/coop/results/applied",
                                      json.dumps({"ids": [rid]}).encode())[1], {"applied": 0})
        self.assertEqual(self.request(self.a, "POST", "/coop/results/applied",
                                      json.dumps({"ids": [rid, "nope", 7]}).encode())[1],
                         {"applied": 1})
        self.assertEqual(self.request(self.a, "GET", "/coop/results")[1], {"results": []})

    def test_the_host_write_must_agree(self):
        self.run_round()
        self.host_writes({self.a: 4})
        world.WORLDS.result(self.a.xuid, 1, 8, 1)  # a client claiming 1st
        self.assertEqual(ledger.LEDGER.pending(self.a.xuid), [])
        world.WORLDS.result(self.a.xuid, 4, 8, 1)  # its report as the host wrote it
        self.assertEqual([e["place"] for e in ledger.LEDGER.pending(self.a.xuid)], [4])

    def test_the_free_roam_s_later_write_does_not_replace_the_race_s(self):
        """Seen with two games: the race's write (4th: 4), then minutes later the world's free
        roam ended for the next event and the host wrote 1 and 0 for the two humans."""
        first = self.run_round()
        self.host_writes({self.a: 4, self.b: 8})
        world.WORLDS.result(self.a.xuid, 4, 8, 1)
        stats.STORE.rated.clear()
        self.host_writes({self.a: 7, self.b: 8})       # the free roam's end: scores 1 and 0
        self.say(self.a, "/event FR05")                 # a second round, worse
        self.server.world_tick()
        self.game_options(self.a, lsp.COOP_HOPPER)
        self.game_options(self.a, lsp.COOP_HOPPER)
        second = max(ledger.LEDGER.rounds.values(), key=lambda r: r["started"])["id"]
        stats.STORE.rated.clear()
        self.host_writes({self.a: 8, self.b: 4})
        world.WORLDS.result(self.a.xuid, 8, 8, 1)
        self.assertEqual([(e["id"], e["place"]) for e in ledger.LEDGER.pending(self.a.xuid)],
                         [(first, 4), (second, 8)])

    def test_without_a_host_write_the_report_counts_and_odd_reports_do_not(self):
        rid = self.run_round()
        world.WORLDS.result(self.a.xuid, 2, 8, 0)
        self.assertEqual(ledger.LEDGER.check(ledger.LEDGER.rounds[rid], "%016X" % self.a.xuid),
                         "not finished")
        world.WORLDS.result(self.a.xuid, 9, 8, 1)
        self.assertEqual(ledger.LEDGER.pending(self.a.xuid), [])
        world.WORLDS.result(self.a.xuid, 2, 8, 1)
        self.assertEqual([e["place"] for e in ledger.LEDGER.pending(self.a.xuid)], [2])

    def test_writes_of_other_sessions_and_reports_out_of_the_window_are_not_taken(self):
        rid = self.run_round()
        self.host_writes({self.a: 6}, session=0x1234)
        self.assertEqual(ledger.LEDGER.rounds[rid]["host"], {})
        ledger.LEDGER.rounds[rid]["started"] -= ledger.ROUND_WINDOW + 1
        self.assertIsNone(ledger.LEDGER.report(self.a.xuid, 1, 8, 1))
        # the free roam's own results (hopper 4, no round) are not the career's either
        self.assertIsNone(ledger.LEDGER.report(self.c.xuid, 1, 2, 1))

    def test_signed_requests_only(self):
        self.run_round()
        status, _ = self.request(self.a, "GET", "/coop/results", key="00" * 16)
        self.assertEqual(status, 403)
        status, _ = self.request(self.a, "POST", "/coop/results/applied", b"[1]")
        self.assertEqual(status, 400)

    def test_the_ledger_survives_a_restart_and_expires(self):
        rid = self.run_round()
        world.WORLDS.result(self.a.xuid, 3, 8, 1)
        again = ledger.Ledger(self.ledger_file)
        again.load()
        self.assertEqual([e["id"] for e in again.pending(self.a.xuid)], [rid])
        again.applied(self.a.xuid, [rid])
        self.assertNotIn(rid, again.rounds)  # applied: settled, dropped
        old = ledger.Ledger(self.ledger_file)
        old.load()
        self.assertEqual(old.rounds, {})


if __name__ == "__main__":
    unittest.main()
