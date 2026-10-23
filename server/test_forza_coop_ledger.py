"""The co-op campaign's ledger (#309 phase B, forza_coop_ledger.py): a campaign round opened by
its host's own call and closed by the world's next round, each player's first own report for
that world's session, the field size from the round, only the requester's game's
XSessionWriteStats, only the requester's result offered to its launcher (signed like the shader
list upload), limits per player, confirmations and the file. Drives the server's handlers
directly (no sockets), with the small game database of test_forza_campaign."""

import json
import os
import struct
import time
import unittest

import forza_chat as ch
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
        """A starts the event, the world's members are known, the round starts (A's game asks
        for its next round twice, with its own key)."""
        self.say(self.a, "/event " + code)
        self.server.world_tick()
        self.game_options(self.a, lsp.COOP_HOPPER)  # the event is the next round
        self.game_options(self.a, lsp.COOP_HOPPER)  # its own prefetch: it started
        return max(ledger.LEDGER.rounds.values(), key=lambda r: r["started"])["id"]

    def end_round(self):
        """The world's free roam after the race: A's game asks for the round after it."""
        self.game_options(self.a, lsp.COOP_HOPPER)

    def stats_write(self, writer, places, session=None):
        """A game's XSessionWriteStats at the race's end: cars - place for each human."""
        session = int(self.sid.hex(), 16) if session is None else session
        stats.STORE.rated.clear()
        for peer, place in places.items():
            body = struct.pack(">BQQB", 1, session, peer.xuid, 1) + struct.pack(">IB", 0xFFFF0000, 1)
            body += struct.pack(">IBi", stats.RELATIVE_SCORE, stats.TYPE_INT32, 8 - place) + bytes(4)
            self.server.on_stats(writer, body)

    def request(self, peer, method, path, body=b"", key=None):
        headers = {"x-xuid": "%016X" % peer.xuid, "x-friends-key": key or self.friends_key(peer)}
        status, _, data = lsp.handle(self.server, method, path, headers, body, ("127.0.0.1", 1))
        return status, json.loads(data)

    def pending(self, peer=None):
        return [(e["code"], e["place"], e["places"]) for e in ledger.LEDGER.pending((peer or self.a).xuid)]

    def test_the_host_s_result_goes_to_its_launcher_once(self):
        rid = self.run_round()
        r = ledger.LEDGER.rounds[rid]
        self.assertEqual((r["event"], r["code"], r["by"], sorted(r["members"]), r["cars"], r["open"]),
                         (4, "FR05", "%016X" % self.a.xuid,
                          sorted("%016X" % p.xuid for p in (self.a, self.b)), 8, True))
        self.stats_write(self.a, {self.a: 3, self.b: 5})
        self.race_report(self.a, 3, self.sid)
        self.race_report(self.b, 5, self.sid)
        status, got = self.request(self.a, "GET", "/coop/results")
        self.assertEqual(status, 200)
        self.assertEqual([(e["id"], e["event"], e["code"], e["place"], e["places"])
                          for e in got["results"]], [(rid, 4, "FR05", 3, 8)])
        # milestone 1: only the requester's career
        self.assertEqual(self.request(self.b, "GET", "/coop/results")[1], {"results": []})
        self.assertEqual(self.request(self.b, "POST", "/coop/results/applied",
                                      json.dumps({"ids": [rid]}).encode())[1], {"applied": 0})
        self.assertEqual(self.request(self.a, "POST", "/coop/results/applied",
                                      json.dumps({"ids": [rid, "nope", 7]}).encode())[1],
                         {"applied": 1})
        self.assertEqual(self.request(self.a, "GET", "/coop/results")[1], {"results": []})

    def test_only_the_player_s_own_first_report_for_the_world_counts(self):
        self.run_round()
        # forged: without the key (an older game, or someone else), with another key
        self.race_report(self.a, 1, self.sid, key=False)
        self.race_report(self.a, 1, self.sid, key="AB" * 16)
        self.assertEqual(self.pending(), [])
        # another world's session, the public Horizon World's playlist
        self.race_report(self.a, 1, b"world-03", key=True)
        self.race_report(self.a, 1, self.sid, hopper=lsp.WORLD_HOPPER)
        self.assertEqual(self.pending(), [])
        # the field is the round's: a claimed smaller grid changes nothing
        self.race_report(self.a, 4, self.sid, places=5)
        self.assertEqual(self.pending(), [("FR05", 4, 8)])
        self.race_report(self.a, 1, self.sid)  # a second report: the first counts
        self.assertEqual(self.pending(), [("FR05", 4, 8)])

    def test_only_the_requester_s_game_writes_the_places_and_its_first_write(self):
        self.run_round()
        self.stats_write(self.b, {self.a: 8})            # a guest's write for the host
        self.stats_write(self.c, {self.a: 8})            # someone outside the world
        self.stats_write(self.a, {self.a: 1, self.b: 2})  # the host's game at the race's end
        self.stats_write(self.a, {self.a: 7, self.b: 8})  # the free roam's end, later
        self.race_report(self.a, 1, self.sid)
        self.assertEqual(self.pending(), [("FR05", 1, 8)])

    def test_the_host_s_write_must_agree_with_its_report(self):
        rid = self.run_round()
        self.stats_write(self.a, {self.a: 4})
        self.race_report(self.a, 1, self.sid)
        self.assertEqual(self.pending(), [])
        self.assertEqual(ledger.LEDGER.check(ledger.LEDGER.rounds[rid], "%016X" % self.a.xuid),
                         "its game wrote place 4")

    def test_reports_after_the_round_closed_go_to_no_round(self):
        first = self.run_round()
        self.end_round()
        self.assertFalse(ledger.LEDGER.rounds[first]["open"])
        self.race_report(self.a, 2, self.sid)              # too late for FR05
        self.assertEqual(self.pending(), [])
        second = self.run_round("FR50")
        self.race_report(self.a, 6, self.sid)
        self.assertEqual([(e["id"], e["code"]) for e in ledger.LEDGER.pending(self.a.xuid)],
                         [(second, "FR50")])

    def test_an_unverified_call_moves_no_campaign_along(self):
        self.say(self.a, "/event FR05")
        self.server.world_tick()
        # someone calling as the host without its key gets the world, and nothing changes
        self.assertEqual(self.game_options(self.a, lsp.COOP_HOPPER, key=False).get("GameTypePreset"), "7")
        self.assertEqual(world.WORLDS.state[self.sid]["state"], "pending")
        self.game_options(self.a, lsp.COOP_HOPPER)
        self.game_options(self.a, lsp.COOP_HOPPER, key=False)
        self.assertEqual(ledger.LEDGER.rounds, {})
        self.game_options(self.a, lsp.COOP_HOPPER)
        rid = next(iter(ledger.LEDGER.rounds))
        self.game_options(self.a, lsp.COOP_HOPPER, key=False)  # no early close either
        self.assertTrue(ledger.LEDGER.rounds[rid]["open"])

    def test_event_is_refused_while_a_round_runs_and_beyond_the_player_s_limit(self):
        self.run_round()
        self.say(self.a, "/event FR50")
        self.assertIn("An event is running in this world", self.notices(self.a)[0])
        self.end_round()
        old = ledger.MAX_ROUNDS_PER_PLAYER
        ledger.MAX_ROUNDS_PER_PLAYER = 1
        try:
            next(iter(ledger.LEDGER.rounds.values()))["results"]["%016X" % self.a.xuid] = \
                {"place": 2, "finished": True, "at": time.time()}
            self.say(self.a, "/event FR50")
            self.assertIn("co-op results waiting for your launcher", self.notices(self.a)[0])
        finally:
            ledger.MAX_ROUNDS_PER_PLAYER = old

    def test_a_player_s_rounds_are_capped_without_touching_others(self):
        lg = ledger.Ledger()
        event = type("E", (), {"id": 4, "code": "FR05", "drivers": 7})
        for n in range(ledger.MAX_ROUNDS_PER_PLAYER + 5):
            lg.start_round(b"world-%02d" % n, event, 1, "A", {1: "A"}, now=1000.0 + n)
            lg.rounds[max(lg.rounds, key=lambda k: lg.rounds[k]["started"])]["results"]["%016X" % 1] = \
                {"place": 1, "finished": True, "at": 1000.0 + n}
        lg.start_round(b"world-bb", event, 2, "B", {2: "B"}, now=900.0)
        lg.rounds[next(k for k, r in lg.rounds.items() if r["by"] == "%016X" % 2)]["results"]["%016X" % 2] = \
            {"place": 1, "finished": True, "at": 900.0}
        lg.expire(now=2000.0)
        by = [r["by"] for r in lg.rounds.values()]
        self.assertEqual((by.count("%016X" % 1), by.count("%016X" % 2)), (ledger.MAX_ROUNDS_PER_PLAYER, 1))

    def test_a_dropped_campaign_is_told(self):
        self.say(self.a, "/event FR05")
        world.WORLDS.state[self.sid]["since"] -= world.PENDING_TIMEOUT + 1
        self.transport.sent.clear()
        self.server.world_tick()
        self.assertEqual(self.notices(self.b), ["FR05 did not start: the world's host must be "
                                                "driving in the free roam. Try /event again"])

    def test_signed_requests_only(self):
        self.run_round()
        status, _ = self.request(self.a, "GET", "/coop/results", key="00" * 16)
        self.assertEqual(status, 403)
        status, _ = self.request(self.a, "POST", "/coop/results/applied", b"[1]")
        self.assertEqual(status, 400)

    def test_the_ledger_is_written_in_batches_and_survives_a_restart(self):
        rid = self.run_round()
        self.race_report(self.a, 3, self.sid)
        self.assertFalse(os.path.exists(self.ledger_file))  # nothing written per report
        ledger.LEDGER.saved -= ledger.SAVE_EVERY
        self.server.world_tick()                              # the tick writes the changes
        again = ledger.Ledger(self.ledger_file)
        again.load()
        self.assertEqual([e["id"] for e in again.pending(self.a.xuid)], [rid])
        again.applied(self.a.xuid, [rid])
        self.assertNotIn(rid, again.rounds)  # settled: dropped


class OperatorAndPlaylistTest(CoopWorldCase):
    def test_world_event_for_all_leaves_co_op_worlds_alone(self):
        sid = self.coop_world()
        self.publish(self.c, lsp.WORLD_HOPPER, b"world-03", 1)
        got = self.server.world_request("all", 0)
        self.assertEqual([r["session"] for r in got["requested"]], [b"world-03".hex()])
        self.assertIsNone(world.WORLDS.state.get(sid))
        self.assertEqual(self.server.world_request(sid.hex(), 4)["requested"], [])

    def test_placement_needs_a_proven_xuid(self):
        self.coop_world()
        self.b.friends_ok = False  # B's HELLO carried someone else's friends key
        self.assertEqual(self.search(self.b, lsp.COOP_HOPPER), [])
        self.b.friends_ok = True
        self.a.friends_ok = False  # nor into a world its leader can't prove
        self.assertEqual(self.search(self.b, lsp.COOP_HOPPER), [])

    def test_no_playlist_without_events(self):
        import forza_campaign as cp
        body = bytes(lsp.x.encode(lsp.x.HopperGetList_Params,
                                  {"maxCategoryResults": 2, "maxHopperResults": 32}))

        def ids():
            results, _ = lsp.x.decode(lsp.x.HopperGetListEx_Results,
                                      lsp.hopper_get_list_ex(self.a.xuid, body)[12:])
            return [h["id"] for h in results["hoppers"]]
        self.assertIn(lsp.COOP_HOPPER, ids())
        cp._events = {}
        self.assertNotIn(lsp.COOP_HOPPER, ids())
        self.say(self.a, "/events")
        self.assertEqual(self.notices(self.a), ["No events for that level"])


if __name__ == "__main__":
    unittest.main()
