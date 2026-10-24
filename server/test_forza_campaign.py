"""The co-op campaign (#309, forza_campaign.py): EventRestrictions decoded as the game does,
a campaign round's GameOptions XML, the Co-op Festival playlist (listed to games that have
it, friends-only placement), /event and /events in WORLD chat, the CPU skill sent to every
console of the world, and a player's result named with the round. Uses a small database
with the game's table layout (no game data); drives the server's handlers directly."""

import os
import sqlite3
import struct
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

import forza_campaign as cp
import forza_chat as ch
import forza_coop_ledger as ledger
import forza_live_lsp as lsp
import forza_live_server as srv
import forza_world as world
import forza_xtsw as x
from test_forza_world_players import FakeTransport, record

ALL = srv.FEATURE_INVITE_PICK | srv.FEATURE_FRIENDS | srv.FEATURE_CHAT | lsp.COOP_FEATURE


def make_db(path):
    """The tables forza_campaign reads, with rows copied from the game's gamedb.slt: FR05
    (event 4: circuit 324, 2 laps, class B, AWD only), FR50 (231: point to point, the
    1960s), a street race (38) and a showcase (8, left out)."""
    db = sqlite3.connect(path)
    db.executescript("""
        CREATE TABLE Events (Id INT, HorizonEventID TEXT, CareerEventStyle INT, Level INT,
            CareerTypeId INT, TargetClass INT, NumberOfDrivers INT, AISkillMed INT,
            AITemperamentMed INT, TimeOfDayStart INT, CrowdSize REAL, CrowdExcitement REAL,
            CashPrize INT, IsDLC INT);
        CREATE TABLE Races (Id INT, EventId INT, RaceNumber INT, TrackId INT, NumLaps INT);
        CREATE TABLE Tracks (id INT, RibbonConfiguration INT, VisibleInMP INT);
        CREATE TABLE EventRestrictions (ID INT, EventID INT, BucketId INT, Value TEXT);
        CREATE TABLE CarClasses (Id INT, BadgeTexturePathPrefix TEXT);
        CREATE TABLE List_CarMake (ID INT, IconPathBase TEXT);
        CREATE TABLE List_DriveType (ID INT, DriveType TEXT);
        INSERT INTO Events VALUES (4, 'FR05', 1, 0, 2, 4, 7, 56, 6, 63540, 1.0, 1.0, 3000, 0),
            (231, 'FR50', 1, 5, 1, 5, 7, 40, 6, 33420, 1.0, 1.0, 15000, 0),
            (38, 'STREET_PLNS_005', 2, 0, 4, 5, 7, 50, 6, 70800, 1.0, 1.0, 35000, 0),
            (8, 'SHOWCASE_01', 3, 0, 7, 4, 1, 50, 6, 43200, 1.0, 1.0, 9000, 0);
        INSERT INTO Races VALUES (1, 4, 1, 324, 2), (2, 231, 1, 145, 1), (3, 38, 1, 1001, 1),
            (4, 8, 1, 145, 1);
        INSERT INTO Tracks VALUES (324, 0, 1), (145, 2, 1), (1001, 6, 1);
        INSERT INTO EventRestrictions VALUES (1, 4, 0, '000000020403'),
            (2, 231, 0, '0003000004000007A8000007B1'), (3, 38, 0, '0000000000');
        INSERT INTO CarClasses VALUES (4, 'CLASS_B'), (5, 'CLASS_A');
        INSERT INTO List_CarMake VALUES (29, 'MINI');
        INSERT INTO List_DriveType VALUES (1, 'FWD'), (3, 'AWD');
    """)
    db.commit()
    db.close()


class RestrictionsTest(unittest.TestCase):
    def test_masks_in_the_game_database(self):
        cases = {
            "0000000000": {},
            "0000000004": {"AllowUpgrades": "true"},
            "000000020403": {"AllowUpgrades": "true", "DriveTypeId": "3"},
            "0003000004000007A8000007B1": {"AllowUpgrades": "true", "YearMin": "1960",
                                           "YearMax": "1969"},
            "0000080004001D": {"AllowUpgrades": "true", "MakeId": "29"},
            "00080002040401": {"AllowUpgrades": "true", "BodyFamilyId": "4", "DriveTypeId": "1"},
            "0002020004000007B90A": {"AllowUpgrades": "true", "YearMax": "1977",
                                     "CountryId": "10"},
            "00800002040203": {"AllowUpgrades": "true", "RegionId": "2", "DriveTypeId": "3"},
            "00000000640000000000003A98": {"AllowUpgrades": "true", "BaseCostMin": "0",
                                           "BaseCostMax": "15000"},
            "0000000024000F4240": {"AllowUpgrades": "true", "BaseCostMin": "1000000"},
            "00002000040007": {"AllowUpgrades": "true", "ModelFamilyId": "7"},
            "0001080004000007BC000E": {"AllowUpgrades": "true", "YearMin": "1980", "MakeId": "14"},
        }
        for value, want in cases.items():
            self.assertEqual(cp.decode_restrictions(value), want, value)

    def test_comparators_floats_and_flags(self):
        # bits 18, 19 (make: DoesNotEqual 7), 6 (WeightMin 1200.0), no AllowUpgrades, RallyOnly
        mask = (1 << 6) | (1 << 18) | (1 << 19) | (1 << 39)
        raw = bytes((mask >> (8 * k)) & 0xFF for k in range(5))
        raw += struct.pack(">f", 1200.0) + bytes([1]) + struct.pack(">H", 7)
        self.assertEqual(cp.decode_restrictions(raw.hex()),
                         {"AllowUpgrades": "false", "RallyOnly": "true", "WeightMin": "1200.000000",
                          "MakeComparator": "DoesNotEqual", "MakeId": "7"})

    def test_power_min_lands_in_power_max_as_in_the_game(self):
        raw = bytes([0x10, 0, 0, 0, 0x04]) + struct.pack(">f", 300.0)
        self.assertEqual(cp.decode_restrictions(raw.hex())["PowerMax"], "300.000000")

    def test_short_values_are_refused(self):
        with self.assertRaises(ValueError):
            cp.decode_restrictions("0003000004000007A8")
        with self.assertRaises(ValueError):
            cp.decode_restrictions("0000")

    def test_time_of_day(self):
        self.assertEqual(cp.time_of_day(63540), "Sunset")    # FR05, 17:39
        self.assertEqual(cp.time_of_day(33420), "Dawn")      # 09:17
        self.assertEqual(cp.time_of_day(43200), "Midday")
        self.assertEqual(cp.time_of_day(85000), "Midnight")  # 23:36, across midnight
        self.assertEqual(cp.time_of_day(70800), "Sunset")    # 19:40


class CampaignDbTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.tmp.name, "gamedb.slt")
        make_db(self.db)
        cp.GAME_DB, cp.STREET, cp.AI_FIELD, cp._events = self.db, True, False, None  # street on

    def tearDown(self):
        cp._events = None
        self.tmp.cleanup()

    def test_events_and_find(self):
        self.assertEqual(sorted(e.id for e in cp.available()), [4, 38, 231])  # no showcase
        self.assertEqual(cp.find("fr05").id, 4)
        self.assertEqual(cp.find("231").code, "FR50")
        self.assertIsNone(cp.find("SHOWCASE_01"))
        self.assertIsNone(cp.find("FR99"))
        cp.STREET = False
        self.assertIsNone(cp.find("STREET_PLNS_005"))
        self.assertEqual(cp.find("FR05").describe(),
                         "FR05: B class, AWD, 7 CPU drivers, 2 laps, Sunset")

    def test_round_xml(self):
        root = ET.fromstring(cp.campaign_xml(cp.find("FR05")))
        self.assertEqual((root.get("GameTypePreset"), root.get("TrackId"), root.get("MaxPlayers"),
                          root.get("MaxAIPlayers"), root.get("AIDifficulty"),
                          root.get("HorizonTimeOfDay"), root.get("UseTraffic")),
                         ("1", "324", "8", "7", "Medium", "Sunset", "false"))
        self.assertIsNone(root.get("AIFieldId"))
        restriction = root.find("Buckets/Bucket/CarRestrictions").attrib
        self.assertEqual(restriction, {"CarClassId": "4", "AllowUpgrades": "true",
                                       "DriveTypeId": "3"})
        self.assertEqual(root.find("StartingGridOptions").get("GridOrderingType"),
                         "PIWithPlayerLast")
        self.assertEqual(root.find("EndConditionOptions").attrib["NumLaps"], "2")
        street = ET.fromstring(cp.campaign_xml(cp.find("STREET_PLNS_005"), laps=1))
        self.assertEqual((street.get("GameTypePreset"), street.get("GameTypeFilterType"),
                          street.get("UseTraffic"), street.find("EndConditionOptions").get("Type")),
                         ("198", "StreetRace", "true", "Checkpoint"))
        cp.AI_FIELD = True
        self.assertEqual(ET.fromstring(cp.campaign_xml(cp.find("FR50"))).get("AIFieldId"), "231")

    def test_street_races_are_opt_in(self):
        import argparse
        parser = argparse.ArgumentParser()
        cp.add_arguments(parser)
        cp.configure(parser.parse_args([]), self.db)
        self.assertEqual(sorted(e.id for e in cp.available()), [4, 231])
        cp.configure(parser.parse_args(["--coop-street"]), self.db)
        self.assertEqual(cp.find("STREET_PLNS_005").describe(),
                         "STREET_PLNS_005: A class, 7 CPU drivers, street race (no traffic online, "
                         "so no career result), Sunset")

    def test_listing(self):
        self.assertEqual(cp.listing(), ["Level 0: FR05", "Level 5: FR50", "Street: STREET_PLNS_005"])
        self.assertEqual(cp.listing(5), ["Level 5: FR50"])
        self.assertEqual(cp.listing("street"), ["Street: STREET_PLNS_005"])
        # lines wrap at `width`; a code longer than that still gets its line
        self.assertEqual(cp.listing("street", width=12), ["Street: STREET_PLNS_005"])
        cp.find("FR50").level = 0
        self.assertEqual(cp.listing(0, width=14), ["Level 0: FR05", "Level 0: FR50"])


class CoopWorldCase(unittest.TestCase):
    """A server with DriverA-C (and an older game, 4) online, the small game database, and
    helpers for the Co-op Festival's messages (also test_forza_coop_ledger.py)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.db = os.path.join(self.tmp.name, "gamedb.slt")
        make_db(self.db)
        cp.GAME_DB, cp.STREET, cp.AI_FIELD, cp._events = self.db, True, False, None  # street on
        world.WORLDS = world.Worlds()
        world.EVENT_LAPS = 0
        ledger.LEDGER = ledger.Ledger()  # in memory
        self.server = srv.ForzaLive("test", 64)
        self.transport = FakeTransport()
        self.server.transport = self.transport
        self.a, self.b, self.c = (self.join(n) for n in (1, 2, 3))
        self.old = self.join(4, features=ALL & ~lsp.COOP_FEATURE)
        self.transport.sent.clear()

    def tearDown(self):
        cp._events = None
        lsp.client_features = None
        world.WORLDS = world.Worlds()
        self.tmp.cleanup()

    def join(self, n, features=ALL):
        body = struct.pack(">HQQIH", srv.VERSION, n, 0x0009000000000000 + n, 0, 0)
        tag = b"Driver%d" % n
        body += bytes([4]) + b"#TU4" + bytes([len(tag)]) + tag + bytes([features])
        body += bytes([16]) + bytes(15) + bytes([n])
        addr = ("127.0.0.1", 20000 + n)
        self.server.datagram_received(srv.MAGIC + bytes([srv.HELLO]) + body, addr)
        return self.server.by_addr.get(addr)

    def publish(self, peer, key, sid, filled):
        body = bytes([1]) + struct.pack(">I", key) + record(sid, filled)
        self.server.on_match(peer, srv.MATCH_PUBLISH, body)

    def standby(self, peer, key, sid):
        body = bytes([2]) + struct.pack(">I", key) + record(sid, 1)
        self.server.on_match(peer, srv.MATCH_PUBLISH, body)

    def search(self, peer, key):
        self.transport.sent.clear()
        self.server.on_match(peer, srv.MATCH_SEARCH, struct.pack(">II", key, 7))
        found = []
        for addr, data in self.transport.sent:
            if addr != peer.addr:
                continue
            body = data[5:]
            count = struct.unpack_from(">H", body, 4)[0] & 0x7FFF
            pos = 6
            for _ in range(count):
                _, _, length = struct.unpack_from(">IQH", body, pos)
                found.append(body[pos + 14:pos + 14 + 8])
                pos += 14 + length
        return found

    def say(self, peer, text, channel=ch.WORLD):
        self.transport.sent.clear()
        body = struct.pack(">BBQ", ch.SAY, channel, 0) + ch.text16(text)
        self.server.datagram_received(srv.MAGIC + bytes([srv.CHAT]) + body, peer.addr)

    def notices(self, peer):
        return [d[8:].decode() for addr, d in self.transport.sent
                if addr == peer.addr and d[4] == srv.CHAT and d[5] == ch.R_NOTICE]

    def messages(self, peer):
        return [d for addr, d in self.transport.sent
                if addr == peer.addr and d[4] == srv.CHAT and d[5] == ch.R_MESSAGE]

    def world_events(self, peer):
        return [d[5:] for addr, d in self.transport.sent
                if addr == peer.addr and d[4] == srv.WORLD_EVENT]

    @staticmethod
    def friends_key(peer):
        """The friends key each test game signed in with (join)."""
        return (bytes(15) + bytes([peer.xuid & 0xFF])).hex()

    def title_call(self, peer, cls, method, params_type, params, key=True):
        """A title service call from peer's game: with its own friends key, without one
        (key=False: an older game, or someone else) or with the given one."""
        headers = {"x-classname": "Forza.WebServices.%s, Forza.WebServices" % cls,
                   "x-method": method, "x-userid": "0x%x" % peer.xuid}
        if key:
            headers["x-friends-key"] = self.friends_key(peer) if key is True else key
        return lsp.service_call(headers, bytes(x.encode(params_type, params)), self.server)

    def game_options(self, peer, hopper, key=True):
        reply = self.title_call(peer, "Hopper", "GetGameOptions", x.HopperGetGameOptions_Params,
                                {"hopperId": hopper, "roundNum": 1, "cchOptionXmlMax": 0x8000}, key)
        results, _ = x.decode(x.HopperGetGameOptions_Results, reply[12:])
        return ET.fromstring(results["gameOptionsXml"])

    def race_report(self, peer, place, sid, finished=True, places=8, hopper=None, key=True,
                    winnings=0):
        """User.WriteRaceStatsEx at a race's end (winnings: its onlineWinnings, before the race
        pays)."""
        stats = {"finished": finished, "userFinishPosition": place, "maxPlaces": places,
                 "hopperId": lsp.COOP_HOPPER if hopper is None else hopper, "sessionId": sid,
                 "onlineWinnings": winnings}
        return self.title_call(peer, "User", "WriteRaceStatsEx", x.UserWriteRaceStats_Head,
                               {"id": 1, "raceStats": stats}, key)

    def coop_world(self):
        """A hosts a Co-op Festival world, B (A's friend) is in it."""
        store = self.server.friends.store
        store.befriend(self.a.xuid, self.b.xuid)
        self.publish(self.a, lsp.COOP_HOPPER, b"coop-001", 2)
        self.standby(self.b, lsp.COOP_HOPPER, b"coop-001")
        return b"coop-001"


class CoopFestivalTest(CoopWorldCase):
    def test_listed_only_to_games_that_have_it(self):
        def ids(peer):
            body = bytes(x.encode(x.HopperGetList_Params,
                                  {"maxCategoryResults": 2, "maxHopperResults": 32}))
            results, _ = x.decode(x.HopperGetListEx_Results,
                                  lsp.hopper_get_list_ex(peer.xuid, body)[12:])
            return [h["id"] for h in results["hoppers"]]
        self.assertEqual(ids(self.a), [lsp.WORLD_HOPPER, 1, 2, lsp.COOP_HOPPER])
        self.assertEqual(ids(self.old), [lsp.WORLD_HOPPER, 1, 2])
        lsp.client_features = None  # no server: unknown, not listed
        self.assertEqual(ids(self.a), [lsp.WORLD_HOPPER, 1, 2])

    def test_placement_only_into_a_friend_s_world(self):
        store = self.server.friends.store
        self.publish(self.a, lsp.COOP_HOPPER, b"coop-001", 1)
        self.publish(self.c, lsp.COOP_HOPPER, b"coop-003", 1)
        self.assertEqual(self.search(self.b, lsp.COOP_HOPPER), [])  # no friends: own world
        store.befriend(self.b.xuid, self.a.xuid)
        self.assertEqual(self.search(self.b, lsp.COOP_HOPPER), [b"coop-001"])
        store.befriend(self.b.xuid, self.c.xuid)
        self.assertEqual(sorted(self.search(self.b, lsp.COOP_HOPPER)), [b"coop-001", b"coop-003"])
        # a pending request is no friendship; the public Horizon World is unchanged
        store.unlink(self.b.xuid, self.c.xuid)
        store.ask(self.b.xuid, self.c.xuid)
        self.assertEqual(self.search(self.b, lsp.COOP_HOPPER), [b"coop-001"])
        self.publish(self.c, lsp.WORLD_HOPPER, b"world-03", 1)
        self.assertEqual(self.search(self.b, lsp.WORLD_HOPPER), [b"world-03"])

    def test_placement_after_the_host_left_follows_the_standby_lead(self):
        store = self.server.friends.store
        sid = self.coop_world()
        self.a.match = None  # A's game closed: B leads the world now
        store.befriend(self.c.xuid, self.a.xuid)
        self.assertEqual(self.search(self.c, lsp.COOP_HOPPER), [])
        store.befriend(self.c.xuid, self.b.xuid)
        self.assertEqual(self.search(self.c, lsp.COOP_HOPPER), [sid])

    def test_event_from_the_host_starts_a_campaign_round(self):
        sid = self.coop_world()
        self.say(self.a, "/event fr05")
        text = "Driver1 starts FR05: B class, AWD, 7 CPU drivers, 2 laps, Sunset"
        self.assertEqual(self.notices(self.a), [text])
        self.assertEqual(self.notices(self.b), [text])
        self.assertEqual(self.messages(self.b), [])  # a command is not relayed
        self.assertEqual(world.WORLDS.pending(sid)[0], 4)
        # the host is told to take it, and every console gets the CPU skill
        self.transport.sent.clear()
        self.server.world_tick()
        self.assertEqual(self.world_events(self.a)[0], struct.pack(">BI", 1, 4) + sid)
        op2 = struct.pack(">BI", 2, 4) + sid + struct.pack(">III", 324, 56, 6)
        self.assertIn(op2, self.world_events(self.a))
        self.assertEqual(self.world_events(self.b), [op2])
        self.assertEqual(self.world_events(self.c), [])
        # the host's next round is the event, with its settings
        root = self.game_options(self.a, lsp.COOP_HOPPER)
        self.assertEqual((root.get("TrackId"), root.get("MaxAIPlayers"),
                          root.find("Buckets/Bucket/CarRestrictions").get("DriveTypeId")),
                         ("324", "7", "3"))
        # the event round starts (its prefetch): the world is next, the CPU skill stops
        self.assertEqual(self.game_options(self.a, lsp.COOP_HOPPER).get("GameTypePreset"), "7")
        self.transport.sent.clear()
        self.server.world_tick()
        self.assertEqual(self.world_events(self.b), [])
        # the players' results are named with the round (and kept by the ledger)
        found = world.WORLDS.result(self.b.xuid, 5, 8, 1, session=sid, trusted=True, coop=True)
        self.assertEqual((found[0], found[1].code), (sid, "FR05"))
        self.assertIsNone(world.WORLDS.result(self.c.xuid, 1, 8, 1))
        # an earlier world's round that still lists the player is not the one reported
        older = dict(world.WORLDS.rounds)
        world.WORLDS.rounds = {b"world-old": (type("E", (), {"id": 231, "code": "FR50"}),
                                              time.monotonic(), {self.b.xuid: "Driver2"}, "Driver1")}
        world.WORLDS.rounds.update(older)
        found = world.WORLDS.result(self.b.xuid, 5, 8, 1, session=sid)
        self.assertEqual((found[0], found[1].code), (sid, "FR05"))

    def test_event_is_refused(self):
        sid = self.coop_world()
        self.say(self.b, "/event FR05")
        self.assertEqual(self.notices(self.b), ["Only the world's host (Driver1) can start an event"])
        self.say(self.a, "/event SHOWCASE_01")
        self.assertIn("No festival or street race", self.notices(self.a)[0])
        self.say(self.a, "/event FR05", channel=ch.SESSION)
        self.assertEqual(self.notices(self.a), ["Type /event in World chat, in a Co-op Festival world"])
        self.say(self.a, "/event FR05")
        self.say(self.a, "/event FR50")
        self.assertEqual(self.notices(self.a), ["An event is already starting in this world"])
        self.assertEqual(world.WORLDS.pending(sid)[0], 4)
        # the public Horizon World takes no /event
        self.publish(self.c, lsp.WORLD_HOPPER, b"world-03", 1)
        self.say(self.c, "/event FR05")
        self.assertEqual(self.notices(self.c), ["Events start from a Co-op Festival world (Find Game)"])
        self.say(self.old, "/event FR05")
        self.assertEqual(self.notices(self.old), ["You are not in a Horizon World"])

    def test_help_and_list(self):
        self.coop_world()
        self.say(self.a, "/event")
        self.assertEqual(self.notices(self.a), [cp.USAGE])
        self.say(self.b, "/events")
        self.assertEqual(self.notices(self.b), ["Level 0: FR05", "Level 5: FR50",
                                                "Street: STREET_PLNS_005"])
        self.say(self.b, "/events 5")
        self.assertEqual(self.notices(self.b), ["Level 5: FR50"])
        # other "/" lines are chat
        self.say(self.a, "/wave")
        self.assertEqual(len(self.messages(self.b)), 1)

    def test_co_op_worlds_get_no_scheduled_events(self):
        self.coop_world()
        self.publish(self.c, lsp.WORLD_HOPPER, b"world-03", 1)
        world.EVENT_MINUTES = 1.0
        world.WORLDS.started = {b"coop-001": time.monotonic() - 120,
                                b"world-03": time.monotonic() - 120}
        try:
            self.server.world_tick()
        finally:
            world.EVENT_MINUTES = 0.0
        self.assertIsNone(world.WORLDS.pending(b"coop-001"))
        self.assertIsNotNone(world.WORLDS.pending(b"world-03"))


if __name__ == "__main__":
    unittest.main()
