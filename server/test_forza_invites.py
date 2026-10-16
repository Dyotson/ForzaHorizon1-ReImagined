"""Game invites (#90): delivered to the invited player only, and dropped with --no-invites.
Drives the server's invite handler directly (no sockets)."""

import struct
import unittest

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
        for n in range(1, 4):
            peer = srv.Peer(n, ("127.0.0.1", 20000 + n), n, 0x0009000000000000 + n,
                            "Driver%d" % n, "#TU4", (0, 0))
            self.server.peers[peer.id] = peer
            self.server.by_addr[peer.addr] = peer
            self.peers.append(peer)

    def invite(self, src, dst):
        self.transport.sent.clear()
        body = struct.pack(">Q", dst.xuid) + bytes(srv.SESSION_INFO_SIZE)
        self.server.on_invite(src, body)
        return [addr for addr, _ in self.transport.sent]

    def test_delivered_to_the_invited_player_only(self):
        a, b, _ = self.peers
        self.assertEqual(self.invite(a, b), [b.addr])

    def test_dropped_when_invites_are_off(self):
        a, b, _ = self.peers
        self.server.invites = False
        self.assertEqual(self.invite(a, b), [])


if __name__ == "__main__":
    unittest.main()
