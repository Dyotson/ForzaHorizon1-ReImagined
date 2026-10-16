"""Game invites (#90): delivered to the invited player only, only between games that pick
and ask (FEATURE_INVITE_PICK), at most INVITE_LIMIT per inviter in INVITE_WINDOW seconds,
and dropped with --no-invites. Drives the server's invite handler directly (no sockets)."""

import struct
import unittest
from unittest import mock

import forza_live_server as srv


class FakeTransport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))


class InvitesTest(unittest.TestCase):
    def setUp(self):
        self.server = srv.ForzaLive("test", 64)
        self.transport = FakeTransport()
        self.server.transport = self.transport
        self.peers = []
        for n in range(1, 8):
            peer = srv.Peer(n, ("127.0.0.1", 20000 + n), n, 0x0009000000000000 + n,
                            "Driver%d" % n, "#TU4", (0, 0))
            peer.features = srv.FEATURE_INVITE_PICK
            self.server.peers[peer.id] = peer
            self.server.by_addr[peer.addr] = peer
            self.peers.append(peer)

    def invite(self, src, dst):
        self.transport.sent.clear()
        body = struct.pack(">Q", dst.xuid) + bytes(srv.SESSION_INFO_SIZE)
        self.server.on_invite(src, body)
        return [addr for addr, _ in self.transport.sent]

    def test_delivered_to_the_invited_player_only(self):
        a, b = self.peers[:2]
        self.assertEqual(self.invite(a, b), [b.addr])

    def test_dropped_when_invites_are_off(self):
        a, b = self.peers[:2]
        self.server.invites = False
        self.assertEqual(self.invite(a, b), [])

    def test_old_games_neither_send_nor_get_invites(self):
        a, b, c = self.peers[:3]
        b.features = 0  # a game up to v0.1.12: joins any invite at once
        self.assertEqual(self.invite(a, b), [])
        self.assertEqual(self.invite(b, c), [])  # and invites everyone online
        self.assertEqual(self.invite(a, c), [c.addr])

    def test_hello_features(self):
        def hello(features):
            body = struct.pack(">HQQIH", srv.VERSION, 7, 0x0009000000000077, 0, 0)
            body += bytes([0]) + bytes([3]) + b"New" + features
            addr = ("127.0.0.1", 30000 + len(features))
            self.server.on_hello(body, addr)
            return self.server.by_addr[addr].features
        self.assertEqual(hello(b""), 0)
        self.assertEqual(hello(bytes([srv.FEATURE_INVITE_PICK])), srv.FEATURE_INVITE_PICK)

    def test_capped_per_inviter(self):
        a, b = self.peers[:2]
        others = self.peers[1:]
        with mock.patch.object(srv.time, "monotonic", return_value=1000.0):
            delivered = [self.invite(a, p) for p in others]
            self.assertEqual(delivered[:srv.INVITE_LIMIT],
                             [[p.addr] for p in others[:srv.INVITE_LIMIT]])
            self.assertEqual(delivered[srv.INVITE_LIMIT:],
                             [[]] * (len(others) - srv.INVITE_LIMIT))
            # Another player's invites are counted on their own.
            self.assertEqual(self.invite(b, a), [a.addr])
        # Once the window has passed, the inviter may invite again.
        with mock.patch.object(srv.time, "monotonic", return_value=1000.0 + srv.INVITE_WINDOW):
            self.assertEqual(self.invite(a, others[-1]), [others[-1].addr])

    def test_unknown_target_does_not_count(self):
        a, b = self.peers[:2]
        stranger = srv.Peer(99, ("127.0.0.1", 29999), 99, 0x0009000000000099, "Gone", "#TU4", (0, 0))
        with mock.patch.object(srv.time, "monotonic", return_value=50.0):
            for _ in range(srv.INVITE_LIMIT + 2):
                self.assertEqual(self.invite(a, stranger), [])
            self.assertEqual(self.invite(a, b), [b.addr])


if __name__ == "__main__":
    unittest.main()
