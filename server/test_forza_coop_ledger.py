"""The co-op campaign's ledger (#309 milestones 1 and 2, forza_coop_ledger.py): a campaign round
opened by its host's own call and closed by the world's next round, each player's first own
report for that world's session, the field size from the round, only the requester's game's
XSessionWriteStats, each player's result offered to its own launcher (signed like the shader
list upload), a guest's place only with the host game's agreeing write, waiting results capped
per player, confirmations and the file. Drives the server's handlers directly (no sockets), with
the small game database of test_forza_campaign."""

import json
import os
import struct
import time
import unittest

import forza_chat as ch
import forza_coop_ledger as ledger
import forza_live_lsp as lsp
import forza_live_server as srv
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

    def stats_write(self, writer, places, session=None, fresh=True):
        """A game's XSessionWriteStats at the race's end: cars - place for each human (fresh:
        the stats store has not rated the session lately)."""
        session = int(self.sid.hex(), 16) if session is None else session
        if fresh:
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

    def test_each_player_s_result_goes_to_its_own_launcher_once(self):
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
        self.assertEqual([(e["id"], e["event"], e["code"], e["place"], e["places"], e["host"])
                          for e in got["results"]], [(rid, 4, "FR05", 3, 8, "Driver1")])
        self.assertEqual(got["results"][0]["until"],
                         int(r["started"] + ledger.KEEP_DAYS * 86400))
        # milestone 2: the guest's own result goes to the guest's career
        self.assertEqual([(e["id"], e["code"], e["place"], e["host"])
                          for e in self.request(self.b, "GET", "/coop/results")[1]["results"]],
                         [(rid, "FR05", 5, "Driver1")])
        self.assertEqual(self.request(self.c, "GET", "/coop/results")[1], {"results": []})
        self.assertEqual(self.request(self.c, "POST", "/coop/results/applied",
                                      json.dumps({"ids": [rid]}).encode())[1], {"applied": 0})
        self.assertEqual(self.request(self.a, "POST", "/coop/results/applied",
                                      json.dumps({"ids": [rid, "nope", 7]}).encode())[1],
                         {"applied": 1})
        self.assertEqual(self.request(self.a, "GET", "/coop/results")[1], {"results": []})
        self.end_round()
        self.assertIn(rid, ledger.LEDGER.rounds)  # the guest's result still waits
        self.assertEqual(self.request(self.b, "POST", "/coop/results/applied",
                                      json.dumps({"ids": [rid]}).encode())[1], {"applied": 1})
        self.assertNotIn(rid, ledger.LEDGER.rounds)  # settled
        self.assertEqual(self.request(self.b, "GET", "/coop/results")[1], {"results": []})

    def test_a_guest_s_place_needs_the_host_s_game_to_agree(self):
        rid = self.run_round()
        self.race_report(self.a, 3, self.sid)
        self.race_report(self.b, 1, self.sid)  # its own report alone is not enough
        r = ledger.LEDGER.rounds[rid]
        self.assertEqual(self.pending(self.b), [])
        self.assertEqual(ledger.LEDGER.check(r, "%016X" % self.b.xuid), "no place from the host's game")
        self.assertEqual(self.pending(self.a), [("FR05", 3, 8)])  # the host's own stands alone
        self.stats_write(self.a, {self.a: 3, self.b: 6})          # the host's game saw B 6th
        self.assertEqual(ledger.LEDGER.check(r, "%016X" % self.b.xuid), "the host's game wrote place 6")
        self.assertEqual(self.pending(self.b), [])
        with self.assertLogs("forza-live", "INFO") as logs:
            self.end_round()
        self.assertTrue(any("Driver2 the host's game wrote place 6" in line for line in logs.output))
        # nothing of B's waits: the round goes once A's result is settled
        self.assertEqual(ledger.LEDGER.waiting_for(self.b.xuid), 0)
        ledger.LEDGER.applied(self.a.xuid, [rid])
        self.assertNotIn(rid, ledger.LEDGER.rounds)

    def test_a_guest_s_agreeing_place_is_offered_with_or_without_the_host_s(self):
        self.run_round()
        self.stats_write(self.a, {self.a: 8, self.b: 2})
        self.race_report(self.b, 2, self.sid)        # the host did not report (it left)
        self.assertEqual(self.pending(self.b), [("FR05", 2, 8)])
        self.assertEqual(self.pending(self.a), [])

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

    def test_the_host_s_write_counts_even_soon_after_the_free_roam_s(self):
        """The stats store skips a session's writes for a minute after rating it; the ledger
        still needs the race's (a guest's place depends on it)."""
        self.stats_write(self.a, {self.a: 8, self.b: 7})              # the free roam's end
        stats.settle(time.monotonic() + stats.SETTLE + 1, force=True)  # rated
        rid = self.run_round()
        self.stats_write(self.a, {self.a: 2, self.b: 4}, fresh=False)  # the race's end, soon
        self.race_report(self.b, 4, self.sid)
        self.assertEqual(ledger.LEDGER.rounds[rid]["host"]["%016X" % self.b.xuid]["score"], 4)
        self.assertEqual(self.pending(self.b), [("FR05", 4, 8)])

    def test_what_the_online_race_paid_is_told_to_the_launcher(self):
        """onlineWinnings in a race's stats is written before that race pays: the round's
        report and the player's next race (any playlist) bracket what the round paid online."""
        rid = self.run_round()
        self.stats_write(self.a, {self.a: 3, self.b: 5})
        self.race_report(self.a, 3, self.sid, winnings=13750)
        self.race_report(self.b, 5, self.sid, winnings=6250)
        self.assertEqual([e["winnings"] for e in ledger.LEDGER.pending(self.a.xuid)], [[13750, None]])
        self.end_round()
        self.race_report(self.a, 4, b"world-03", hopper=lsp.WORLD_HOPPER, winnings=16250)
        self.race_report(self.a, 2, b"lobby-01", hopper=1, key=False, winnings=99999)  # not its own
        self.assertEqual([(e["id"], e["winnings"]) for e in ledger.LEDGER.pending(self.a.xuid)],
                         [(rid, [13750, 16250])])
        at = ledger.LEDGER.pending(self.a.xuid)[0]["winnings_at"]  # when: the launcher checks the save
        self.assertLessEqual(abs(at - time.time()), 5)
        self.assertNotIn("winnings_at", ledger.LEDGER.pending(self.b.xuid)[0])
        self.assertEqual([e["winnings"] for e in ledger.LEDGER.pending(self.b.xuid)], [[6250, None]])

    def test_a_street_race_gives_no_career_result(self):
        """Online races run without traffic (the multiplayer race flow turns it off), so a street
        race (--coop-street) is not the career's: its results go to nobody's career."""
        rid = self.run_round("STREET_PLNS_005")
        r = ledger.LEDGER.rounds[rid]
        self.stats_write(self.a, {self.a: 1, self.b: 2})
        self.race_report(self.a, 1, self.sid)
        self.race_report(self.b, 2, self.sid)
        self.assertEqual((r["street"], self.pending(self.a), self.pending(self.b)), (True, [], []))
        self.assertEqual(ledger.LEDGER.check(r, "%016X" % self.a.xuid), "a street race without traffic")
        self.end_round()
        self.assertNotIn(rid, ledger.LEDGER.rounds)

    def test_a_host_write_from_an_unproven_xuid_is_ignored(self):
        """A second client that claims the host's XUID without its friends key can't confirm a
        guest's place (nor deny it): only the host's proven game counts."""
        body = struct.pack(">HQQIH", srv.VERSION, 99, self.a.xuid, 0, 0)
        body += bytes([4]) + b"#TU4" + bytes([7]) + b"DriverA" + bytes([lsp.COOP_FEATURE | 0x04])
        body += bytes([16]) + bytes(16)  # not A's key
        self.server.datagram_received(srv.MAGIC + bytes([srv.HELLO]) + body, ("127.0.0.1", 20099))
        fake = self.server.by_addr.get(("127.0.0.1", 20099))
        self.assertFalse(fake.friends_ok)
        rid = self.run_round()
        self.stats_write(fake, {self.b: 1})             # "the host's game saw B 1st"
        self.race_report(self.b, 1, self.sid)
        self.assertEqual(ledger.LEDGER.rounds[rid]["host"], {})
        self.assertEqual(self.pending(self.b), [])
        self.stats_write(self.a, {self.a: 7, self.b: 8})  # the real host's game
        self.assertEqual(ledger.LEDGER.check(ledger.LEDGER.rounds[rid], "%016X" % self.b.xuid),
                         "the host's game wrote place 8")

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
        self.assertNotIn(first, ledger.LEDGER.rounds)      # closed without a result: gone
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
        self.stats_write(self.a, {self.a: 2, self.b: 3})
        self.race_report(self.b, 3, self.sid)
        self.end_round()
        old = ledger.MAX_WAITING
        ledger.MAX_WAITING = 1
        try:
            # the guest's waiting result does not hold up the host
            self.say(self.a, "/event FR50")
            self.assertIn("starts FR50", self.notices(self.a)[0])
            world.WORLDS.state.clear()
            self.transport.sent.clear()
            next(iter(ledger.LEDGER.rounds.values()))["results"]["%016X" % self.a.xuid] = \
                {"place": 2, "finished": True, "at": time.time()}
            self.say(self.a, "/event FR50")
            until = time.strftime("%d %b %Y", time.gmtime(
                next(iter(ledger.LEDGER.rounds.values()))["started"] + ledger.KEEP_DAYS * 86400))
            self.assertEqual(self.notices(self.a)[0],
                             "1 results of co-op events you started still wait for your launcher "
                             "(an event your career has not unlocked keeps its result until it is "
                             "unlocked). The oldest expires on %s; start more events after that, "
                             "or after your launcher has applied some" % until)
        finally:
            ledger.MAX_WAITING = old

    def test_a_player_s_waiting_results_are_capped_without_touching_others(self):
        lg = ledger.Ledger()
        event = type("E", (), {"id": 4, "code": "FR05", "drivers": 7})
        guest, t0 = "%016X" % 2, time.time() - 3600
        for n in range(ledger.MAX_WAITING + 5):  # host 1's rounds: only its guest 2 reported
            world_sid = b"world-%02d" % n
            lg.start_round(world_sid, event, 1, "A", {1: "A", 2: "B"}, now=t0 + n)
            lg.report(2, world_sid, 1, True, now=t0 + n)
            lg.host_score(1, int(world_sid.hex(), 16), 2, 7, now=t0 + n)
            lg.close_round(world_sid)
        first = lg.start_round(b"world-cc", event, 3, "C", {3: "C"}, now=t0 - 60)
        lg.report(3, b"world-cc", 4, True, now=t0 - 60)
        lg.close_round(b"world-cc")
        lg.expire()
        self.assertEqual(sum(1 for r in lg.rounds.values() if guest in r["results"]), ledger.MAX_WAITING)
        self.assertEqual([e["place"] for e in lg.pending(2)], [1] * ledger.MAX_WAITING)
        self.assertEqual(min(e["at"] for e in lg.pending(2)), int(t0 + 5))  # its oldest five went
        self.assertEqual([e["id"] for e in lg.pending(3)], [first])  # an older one of another
        # /event counts the rounds a player asked for: 2's guest results never block it
        self.assertEqual((lg.waiting_for(1), lg.waiting_for(2), lg.waiting_for(3)), (0, 0, 1))

    def test_the_cap_drops_guest_results_before_the_player_s_own(self):
        lg = ledger.Ledger()
        event = type("E", (), {"id": 4, "code": "FR05", "drivers": 7})
        t0 = time.time() - 3600
        for n in range(ledger.MAX_WAITING):  # 2 asks for 20 rounds, the oldest results
            sid = b"own-%02d" % n
            lg.start_round(sid, event, 2, "B", {2: "B"}, now=t0 + n)
            lg.report(2, sid, 3, True, now=t0 + n)
            lg.close_round(sid)
        sid = b"guest-01"  # then a newer round as 1's guest
        lg.start_round(sid, event, 1, "A", {1: "A", 2: "B"}, now=t0 + 100)
        lg.report(2, sid, 3, True, now=t0 + 100)
        lg.host_score(1, int(sid.hex(), 16), 2, 5, now=t0 + 100)
        lg.close_round(sid)
        self.assertEqual(sorted({r["by"] for r in lg.rounds.values() if "%016X" % 2 in r["results"]}),
                         ["%016X" % 2])  # the guest result went, all 20 own ones stay
        self.assertEqual(lg.waiting_for(2), ledger.MAX_WAITING)

    def test_an_open_round_s_result_does_not_push_out_a_valid_one(self):
        """A report in a round still open may never become grantable (not finished, no host
        write): it doesn't count toward the cap until its round closes grantable."""
        lg = ledger.Ledger()
        event = type("E", (), {"id": 4, "code": "FR05", "drivers": 7})
        t0 = time.time() - 600  # the last round is open (OPEN_LIMIT)
        for n in range(ledger.MAX_WAITING):
            sid = b"world-%02d" % n
            lg.start_round(sid, event, 1, "A", {1: "A", 2: "B"}, now=t0 + n)
            lg.report(2, sid, 2, True, now=t0 + n)
            lg.host_score(1, int(sid.hex(), 16), 2, 6, now=t0 + n)
            lg.close_round(sid)
        lg.start_round(b"world-dn", event, 1, "A", {1: "A", 2: "B"}, now=t0 + 100)
        lg.report(2, b"world-dn", 8, False, now=t0 + 100)  # did not finish
        lg.start_round(b"other-01", event, 3, "C", {3: "C"})  # another world: expire() runs
        self.assertEqual(len(lg.pending(2)), ledger.MAX_WAITING)
        lg.close_round(b"world-dn")
        self.assertEqual(len(lg.pending(2)), ledger.MAX_WAITING)

    def test_a_result_waits_at_most_keep_days(self):
        rid = self.run_round()
        self.stats_write(self.a, {self.a: 2, self.b: 3})
        self.race_report(self.b, 3, self.sid)
        self.end_round()
        ledger.LEDGER.expire(now=time.time() + ledger.KEEP_DAYS * 86400 - 60)
        self.assertIn(rid, ledger.LEDGER.rounds)
        ledger.LEDGER.expire(now=time.time() + ledger.KEEP_DAYS * 86400 + 60)
        self.assertNotIn(rid, ledger.LEDGER.rounds)

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
        self.assertIn(rid, again.rounds)     # open: the guest may still report
        again.close_round(self.sid)
        self.assertNotIn(rid, again.rounds)  # settled: dropped

    def test_a_milestone_1_file_still_loads(self):
        rid = self.run_round()
        self.race_report(self.a, 3, self.sid)
        r = dict(ledger.LEDGER.rounds[rid])
        del r["dropped"]
        with open(self.ledger_file, "w") as f:
            json.dump({"version": 2, "rounds": [r]}, f)
        again = ledger.Ledger(self.ledger_file)
        again.load()
        self.assertEqual([e["id"] for e in again.pending(self.a.xuid)], [rid])
        self.assertEqual(again.rounds[rid]["dropped"], [])


    def test_a_failed_write_is_tried_again(self):
        rid = self.run_round()
        self.race_report(self.a, 3, self.sid)
        real = os.replace
        calls = []
        def fail_once(src, dst):
            calls.append(dst)
            if len(calls) == 1:
                raise PermissionError("sharing violation")
            return real(src, dst)
        ledger.os.replace = fail_once
        self.addCleanup(setattr, ledger.os, "replace", real)
        ledger.LEDGER.saved -= ledger.SAVE_EVERY
        with self.assertLogs("forza-live", "WARNING"):
            self.server.world_tick()                          # the write fails
        self.assertTrue(ledger.LEDGER.dirty)                  # still to be written
        ledger.LEDGER.saved -= ledger.SAVE_EVERY
        self.server.world_tick()                              # and the next tick writes it
        self.assertFalse(ledger.LEDGER.dirty)
        self.assertEqual(len(calls), 2)
        again = ledger.Ledger(self.ledger_file)
        again.load()
        self.assertEqual([e["id"] for e in again.pending(self.a.xuid)], [rid])

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
