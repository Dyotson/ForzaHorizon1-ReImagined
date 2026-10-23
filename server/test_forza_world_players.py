"""The Horizon World with three or more players (issue #12): placement into the fullest
world, the standby host after the host left (also before the gone host timed out), the AI
level from the number of humans. Drives the server's matchmaking handlers directly (no
sockets)."""

import struct
import unittest
from unittest import mock

import forza_live_lsp as lsp
import forza_live_server as srv
import forza_world as world

KEY = lsp.WORLD_HOPPER


class FakeTransport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))


def record(session_id, filled, slots=8):
    """A session record: the XSESSION_INFO (the session id first), then the public slots."""
    info = bytearray(srv.SESSION_INFO_SIZE)
    info[:8] = session_id
    return bytes(info) + bytes([slots, 0, filled, 0]) + bytes(32)


class WorldPlayersTest(unittest.TestCase):
    def setUp(self):
        self.server = srv.ForzaLive("test", 64)
        self.transport = FakeTransport()
        self.server.transport = self.transport
        self.n = 0

    def player(self, name):
        self.n += 1
        peer = srv.Peer(self.n, ("127.0.0.1", 20000 + self.n), self.n, 0x0009000000000000 + self.n,
                        name, "#TU4", (0, 0))
        self.server.peers[peer.id] = peer
        self.server.by_addr[peer.addr] = peer
        return peer

    def publish(self, peer, sid, filled):
        body = bytes([1]) + struct.pack(">I", KEY) + record(sid, filled)
        self.server.on_match(peer, srv.MATCH_PUBLISH, body)

    def standby(self, peer, sid, host=False):
        body = bytes([4 if host else 2]) + struct.pack(">I", KEY) + record(sid, 1)
        self.server.on_match(peer, srv.MATCH_PUBLISH, body)

    def search(self, peer):
        """The session ids the search answers, in order, with their filled slots."""
        self.transport.sent.clear()
        self.server.on_match(peer, srv.MATCH_SEARCH, struct.pack(">II", KEY, 7))
        found = []
        for addr, data in self.transport.sent:
            if addr != peer.addr:
                continue
            body = data[5:]
            count = struct.unpack_from(">H", body, 4)[0] & 0x7FFF
            pos = 6
            for _ in range(count):
                _, _, length = struct.unpack_from(">IQH", body, pos)
                rec = body[pos + 14:pos + 14 + length]
                found.append((rec[:8], rec[srv.SESSION_INFO_SIZE + 2]))
                pos += 14 + length
        return found

    def test_status_names_each_lobby_s_playlist_and_custom_game_type(self):
        a, b, c = self.player("A"), self.player("B"), self.player("C")
        self.publish(a, b"world-01", 1)
        self.standby(b, b"world-01")
        # a public custom Street Race: user properties (count, then id u32, type u8, value
        # 8 bytes) carry 0x10000030 = 4, the game type
        props = bytes([2]) + struct.pack(">IB", 0x10000039, 1) + struct.pack(">I", 9) + bytes(4) \
            + struct.pack(">IB", 0x10000030, 1) + struct.pack(">I", 4) + bytes(4)
        rec = record(b"street01", 3)[:srv.SESSION_INFO_SIZE + 4] + props + bytes(3)
        self.server.on_match(c, srv.MATCH_PUBLISH, bytes([1]) + struct.pack(">I", 4) + rec)
        peers = {p["gamertag"]: p for p in self.server.status()["rooms"]["#TU4"]}
        self.assertEqual(peers["A"]["match"], {"playlist": KEY, "session": b"world-01".hex(),
                                               "players": 1, "slots": 8, "game_type": None})
        self.assertEqual(peers["B"]["standby"]["session"], b"world-01".hex())
        self.assertIsNone(peers["B"]["match"])
        self.assertEqual(peers["C"]["match"]["game_type"], 4)
        self.assertEqual(peers["C"]["match"]["players"], 3)
        self.assertIsNone(srv.ForzaLive.match_game_type(rec[:srv.SESSION_INFO_SIZE + 10]))

    def test_third_player_is_placed_into_the_world_with_two(self):
        a, b, c = self.player("A"), self.player("B"), self.player("C")
        self.publish(a, b"\x01" * 8, 2)  # the host and one joiner
        self.assertEqual(self.search(c), [(b"\x01" * 8, 2)])
        self.assertEqual(self.server.world_sessions()[b"\x01" * 8][1], 2)
        self.publish(a, b"\x01" * 8, 3)
        self.assertEqual(self.server.world_sessions()[b"\x01" * 8][1], 3)

    def test_fullest_world_first_and_full_worlds_are_not_offered(self):
        h1, h2, h3, new = (self.player(n) for n in ("H1", "H2", "H3", "N"))
        self.publish(h1, b"\x01" * 8, 1)
        self.publish(h2, b"\x02" * 8, 5)
        self.publish(h3, b"\x03" * 8, 8)  # full
        self.assertEqual(self.search(new), [(b"\x02" * 8, 5), (b"\x01" * 8, 1)])

    def test_host_leaves_with_two_others_the_game_s_host_leads_the_world(self):
        host, b, c, d = (self.player(n) for n in ("Host", "B", "C", "D"))
        sid = b"\x07" * 8
        self.publish(host, sid, 3)
        self.standby(b, sid)
        self.standby(c, sid)
        self.server.drop(host, "left")
        self.standby(c, sid, host=True)  # the game made C its host
        self.assertEqual(self.search(d), [(sid, 2)])  # the two remaining players
        led = self.server.world_sessions()[sid]
        self.assertEqual((led[0].name, led[1]), ("C", 2))
        # the searcher is placed with the remaining players' record: C's own address
        self.transport.sent.clear()
        self.server.on_match(d, srv.MATCH_SEARCH, struct.pack(">II", KEY, 9))
        body = self.transport.sent[0][1][5:]
        self.assertEqual(struct.unpack_from(">I", body, 6)[0], c.id)

    def test_without_the_game_s_host_the_oldest_member_leads(self):
        host, b, c = (self.player(n) for n in ("Host", "B", "C"))
        sid = b"\x08" * 8
        self.publish(host, sid, 3)
        self.standby(b, sid)
        self.standby(c, sid)
        self.server.drop(host, "left")
        self.assertEqual(self.server.world_sessions()[sid][0].name, "B")

    def first_host(self, searcher):
        """The player id of the first session a search by searcher answers."""
        self.transport.sent.clear()
        self.server.on_match(searcher, srv.MATCH_SEARCH, struct.pack(">II", KEY, 9))
        return struct.unpack_from(">I", self.transport.sent[0][1][5:], 6)[0]

    def test_a_closed_host_s_record_gives_way_to_the_game_s_new_host(self):
        # The host's game was closed: it is still registered (until it times out), and so
        # is its record, when the game makes C its host. Searchers go to C, not to the
        # gone host (seen with four games: the searcher opened a world of its own).
        host, b, c, d = (self.player(n) for n in ("Host", "B", "C", "D"))
        sid = b"\x0a" * 8
        self.publish(host, sid, 3)
        self.standby(b, sid)
        self.standby(c, sid)
        self.assertEqual(self.first_host(d), host.id)
        self.standby(c, sid, host=True)
        self.assertEqual(self.search(d), [(sid, 2)])
        self.assertEqual(self.first_host(d), c.id)
        self.assertEqual(self.server.world_sessions()[sid][0].name, "C")  # world events go to C

    def test_the_latest_of_the_game_s_hosts_leads(self):
        # B became the host when the first host left; then B's game was closed and the
        # game made C its host while B was still registered.
        host, b, c, d, e = (self.player(n) for n in ("Host", "B", "C", "D", "E"))
        sid = b"\x0b" * 8
        self.publish(host, sid, 4)
        for p in (b, c, d):
            self.standby(p, sid)
        self.server.drop(host, "left")
        with mock.patch("forza_world_host.time.monotonic", return_value=100.0):
            self.standby(b, sid, host=True)
        self.assertEqual(self.first_host(e), b.id)
        with mock.patch("forza_world_host.time.monotonic", return_value=130.0):
            self.standby(c, sid, host=True)
        self.assertEqual(self.first_host(e), c.id)
        self.assertEqual(self.server.world_sessions()[sid][0].name, "C")
        self.standby(b, sid, host=True)  # B's repeated op 4 does not take it back
        self.assertEqual(self.first_host(e), c.id)
        self.server.drop(b, "timed out")
        self.assertEqual(self.server.world_sessions()[sid], (c, 2))

    def test_a_host_claim_in_another_world_leaves_this_one_alone(self):
        h1, h2, m2, d = (self.player(n) for n in ("H1", "H2", "M2", "D"))
        self.publish(h1, b"\x0c" * 8, 3)
        self.publish(h2, b"\x0d" * 8, 2)
        self.standby(m2, b"\x0e" * 8, host=True)  # hosts a third world, left by its host
        self.assertEqual([s for s, _ in self.search(d)], [b"\x0c" * 8, b"\x0d" * 8, b"\x0e" * 8])
        self.assertEqual(self.server.world_sessions()[b"\x0c" * 8][0], h1)

    def test_a_world_without_players_disappears(self):
        host, b = self.player("Host"), self.player("B")
        sid = b"\x09" * 8
        self.publish(host, sid, 2)
        self.standby(b, sid)
        self.server.drop(host, "left")
        self.server.drop(b, "left")
        self.assertEqual(self.server.world_sessions(), {})

    def test_ai_difficulty_follows_the_human_share_of_the_grid(self):
        levels = [world.ai_difficulty(n) for n in range(1, 9)]
        self.assertEqual(levels, ["Easy", "Easy", "Medium", "Medium", "Medium", "Hard", "Hard", "Hard"])


if __name__ == "__main__":
    unittest.main()
