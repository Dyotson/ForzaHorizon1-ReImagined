"""Find Custom Game lists a lobby whose host left under its current game type and details
(issue #391): a member's standby record goes out with the newest description of the lobby
from its host or from the game's new host, not the one the member found it with. Drives the
server's matchmaking handlers directly (no sockets)."""

import struct
import unittest

import forza_live_server as srv

INFECTED, KING = 5, 7  # Find Custom Game's search keys: the game type (property 0x10000030)


class FakeTransport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))


def record(session_id, details, filled=1, game_type=INFECTED):
    """A session record: XSESSION_INFO (the session id first), the public slots, the user
    properties (0x10000030 = the game type), no contexts, then the QoS data (Find Custom
    Game's details; here just a label)."""
    info = bytearray(srv.SESSION_INFO_SIZE)
    info[:8] = session_id
    props = bytes([1]) + struct.pack(">IB", 0x10000030, 1) + struct.pack(">I", game_type) + bytes(4)
    return (bytes(info) + bytes([8, 0, filled, 0]) + props + bytes([0])
            + struct.pack(">H", len(details)) + details)


class LobbyDetailsTest(unittest.TestCase):
    def setUp(self):
        self.server = srv.ForzaLive("test", 64)
        self.transport = FakeTransport()
        self.server.transport = self.transport
        self.n = 0

    def player(self, name, features=srv.FEATURE_LOBBY):
        self.n += 1
        peer = srv.Peer(self.n, ("127.0.0.1", 20000 + self.n), self.n, 0x0009000000000000 + self.n,
                        name, "#TU4", (0, 0))
        peer.features = features
        self.server.peers[peer.id] = peer
        self.server.by_addr[peer.addr] = peer
        return peer

    def host(self, peer, sid, key, details, filled=1):
        body = bytes([1]) + struct.pack(">I", key) + record(sid, details, filled, key)
        self.server.on_match(peer, srv.MATCH_PUBLISH, body)

    def standby(self, peer, sid, key, details, game_host=False):
        body = bytes([4 if game_host else 2]) + struct.pack(">I", key) + record(sid, details, 1, key)
        self.server.on_match(peer, srv.MATCH_PUBLISH, body)

    def search(self, peer, key):
        """[(player id, session id, details)] of the sessions a search for key answers."""
        self.transport.sent.clear()
        self.server.on_match(peer, srv.MATCH_SEARCH, struct.pack(">II", key, 7))
        found = []
        for addr, data in self.transport.sent:
            body = data[5:]
            count = struct.unpack_from(">H", body, 4)[0] & 0x7FFF
            pos = 6
            for _ in range(count):
                pid, _, length = struct.unpack_from(">IQH", body, pos)
                rec = body[pos + 14:pos + 14 + length]
                found.append((pid, rec[:8], rec[-self.details_size(rec):]))
                pos += 14 + length
        return found

    @staticmethod
    def details_size(rec):
        pos = srv.SESSION_INFO_SIZE + 4
        pos += 1 + 13 * rec[pos]
        pos += 1 + 8 * rec[pos]
        return struct.unpack_from(">H", rec, pos)[0]

    def test_a_lobby_whose_host_left_is_listed_as_its_host_last_described_it(self):
        a, b, c = self.player("A"), self.player("B"), self.player("C")
        sid = b"lobby001"
        self.host(a, sid, INFECTED, b"INFECTED 10:00 Steelworks host A")
        self.standby(b, sid, INFECTED, b"INFECTED 10:00 Steelworks host A")  # found by B
        self.host(a, sid, KING, b"KING 03:00 Steelworks host A", filled=2)  # the next setup
        # A leaves the lobby: its game releases the description, then the session
        self.host(a, sid, KING, b"", filled=1)
        self.server.on_match(a, srv.MATCH_PUBLISH, bytes([0]))
        self.assertEqual(self.search(c, INFECTED), [])
        self.assertEqual(self.search(c, KING), [(b.id, sid, b"KING 03:00 Steelworks host A")])

    def test_the_game_s_new_host_describes_the_lobby_for_the_others(self):
        a, b, c, d = (self.player(n) for n in "ABCD")
        sid = b"lobby002"
        self.host(a, sid, INFECTED, b"INFECTED host A")
        self.standby(b, sid, INFECTED, b"INFECTED host A")
        self.standby(c, sid, INFECTED, b"INFECTED host A")
        self.server.drop(a, "left")
        self.standby(c, sid, KING, b"KING host C", game_host=True)  # C's game hosts it now
        self.assertEqual(self.search(d, KING), [(c.id, sid, b"KING host C")])
        # C leaves the server as well: B (still on the key and details it found the lobby
        # with) stands in for the lobby with C's description
        self.server.drop(c, "left")
        self.assertEqual(self.search(d, INFECTED), [])
        self.assertEqual(self.search(d, KING), [(b.id, sid, b"KING host C")])

    def test_an_older_game_s_host_record_gets_the_host_s_last_description(self):
        a, d = self.player("A"), self.player("D")
        b = self.player("B", features=0x0F)  # v0.1.33: no FEATURE_LOBBY
        sid = b"lobby007"
        self.host(a, sid, INFECTED, b"INFECTED host A")
        self.standby(b, sid, INFECTED, b"INFECTED host A")
        self.host(a, sid, KING, b"KING host A", filled=2)
        self.host(a, sid, KING, b"", filled=1)
        self.server.drop(a, "left")
        # the game made B its host; B's game still sends what it found the lobby with
        self.standby(b, sid, INFECTED, b"INFECTED host A", game_host=True)
        self.assertEqual(self.search(d, INFECTED), [])
        self.assertEqual(self.search(d, KING), [(b.id, sid, b"KING host A")])
        self.assertEqual(self.server.lobby_tails[sid][0], KING)  # B's record noted nothing

    def test_without_a_host_s_description_a_member_s_record_goes_out_as_it_is(self):
        b, d = self.player("B"), self.player("D")
        sid = b"lobby003"
        self.standby(b, sid, INFECTED, b"INFECTED as found")
        self.assertEqual(self.search(d, INFECTED), [(b.id, sid, b"INFECTED as found")])

    def test_a_hosted_lobby_still_goes_out_as_its_host_publishes_it(self):
        a, b, d = self.player("A"), self.player("B"), self.player("D")
        sid = b"lobby004"
        self.host(a, sid, KING, b"KING host A", filled=2)
        self.standby(b, sid, INFECTED, b"INFECTED as found")
        self.assertEqual(self.search(d, KING), [(a.id, sid, b"KING host A")])
        self.assertEqual(self.search(d, INFECTED), [])

    def test_descriptions_of_lobbies_nobody_lists_are_forgotten(self):
        a, b = self.player("A"), self.player("B")
        self.host(a, b"lobby005", KING, b"KING")
        self.standby(b, b"lobby006", KING, b"KING", game_host=True)
        self.server.expire()
        self.assertEqual(set(self.server.lobby_tails), {b"lobby005", b"lobby006"})
        self.server.on_match(a, srv.MATCH_PUBLISH, bytes([0]))
        self.server.on_match(b, srv.MATCH_PUBLISH, bytes([3]))
        self.server.expire()
        self.assertEqual(self.server.lobby_tails, {})

    def test_the_number_of_descriptions_is_bounded(self):
        a = self.player("A")
        for i in range(srv.MAX_LOBBY_TAILS + 10):
            self.host(a, struct.pack(">Q", i), KING, b"KING")
        self.assertEqual(len(self.server.lobby_tails), srv.MAX_LOBBY_TAILS)
        self.assertIn(struct.pack(">Q", srv.MAX_LOBBY_TAILS + 9), self.server.lobby_tails)
        self.assertNotIn(struct.pack(">Q", 0), self.server.lobby_tails)


if __name__ == "__main__":
    unittest.main()
