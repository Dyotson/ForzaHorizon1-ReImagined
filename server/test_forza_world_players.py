"""The Horizon World with three or more players (issue #12): placement into the fullest
world, the standby host after the host left, the AI level from the number of humans.
Drives the server's matchmaking handlers directly (no sockets)."""

import struct
import unittest

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
