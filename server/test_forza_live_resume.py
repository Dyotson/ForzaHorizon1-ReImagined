"""A game that went silent (a Steam Deck that slept, a stopped game: issue #140) is dropped after
PEER_TIMEOUT, and when it says HELLO again with its token and XUID it gets its old player id
back, so its XNADDR and the session it hosts stay the same. Drives the datagram handler
directly (no sockets)."""

import unittest
from unittest.mock import patch

import forza_live_server as srv
from test_forza_live_restart import FakeTransport, hello, welcome_id


class ResumeTest(unittest.TestCase):
    def setUp(self):
        self.server = srv.ForzaLive("test", 64)
        self.server.transport = FakeTransport()
        self.addr = {n: ("127.0.0.1", 30000 + i) for i, n in enumerate("AB")}
        self.token = {"A": 0x1111, "B": 0x2222}
        self.xuid = {"A": 0x0009000000000001, "B": 0x0009000000000002}

    def join(self, who):
        self.server.datagram_received(hello(self.token[who], self.xuid[who], "player " + who),
                                      self.addr[who])
        return welcome_id(self.server.transport, self.addr[who])[0]

    def silence(self, who, seconds=srv.PEER_TIMEOUT + 1):
        for peer in self.server.peers.values():
            if peer.token == self.token[who]:
                peer.seen -= seconds
        self.server.expire()

    def test_back_after_a_timeout_with_the_same_id(self):
        a = self.join("A")
        self.silence("A")
        self.assertEqual(self.server.peers, {})
        with self.assertLogs("forza-live", "INFO") as logs:
            self.assertEqual(self.join("A"), a)
        self.assertTrue(any("back  #%d " % a in line for line in logs.output))
        self.assertEqual(self.server.gone, {})

    def test_back_from_another_address(self):
        a = self.join("A")
        self.silence("A")
        self.addr["A"] = ("127.0.0.1", 40000)  # the NAT gave another port while it slept
        self.assertEqual(self.join("A"), a)
        self.assertEqual(self.server.per_ip, {"127.0.0.1": 1})

    def test_the_id_is_not_given_to_another_player(self):
        a = self.join("A")
        self.silence("A")
        self.server.next_id = a  # the next new player would get exactly that id
        b = self.join("B")
        self.assertNotEqual(b, a)
        self.assertEqual(self.join("A"), a)

    def test_needs_the_token_and_xuid(self):
        a = self.join("A")
        self.silence("A")
        self.xuid["A"] += 1
        self.assertNotEqual(self.join("A"), a)
        a2 = self.join("B")
        self.silence("B")
        self.token["B"] = 0x9999  # a new game process
        self.assertNotEqual(self.join("B"), a2)

    def test_only_for_silence(self):
        a = self.join("A")
        self.server.datagram_received(srv.MAGIC + bytes([srv.BYE]), self.addr["A"])
        self.assertEqual(self.server.gone, {})
        self.assertNotEqual(self.join("A"), a)

    def test_forgotten_after_a_while(self):
        a = self.join("A")
        self.silence("A")
        token = self.token["A"]
        pid, xuid, when = self.server.gone[token]
        self.server.gone[token] = (pid, xuid, when - srv.GONE_KEEP - 1)
        self.server.expire()
        self.assertEqual(self.server.gone, {})
        self.assertNotEqual(self.join("A"), a)

    def test_bounded(self):
        with patch.object(srv, "GONE_MAX", 2):
            for token in (1, 2, 3):
                self.token["A"] = token
                self.join("A")
                self.silence("A")
            self.assertEqual(sorted(self.server.gone), [2, 3])


if __name__ == "__main__":
    unittest.main()
