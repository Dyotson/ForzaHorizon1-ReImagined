"""Text chat (#92, forza_chat.py): the session, world and direct channels, the length cap,
rate limit, blocks and mutes, the admin's mute and kick, the moderation ring and old games.
Drives the server's handlers directly (no sockets)."""

import os
import struct
import tempfile
import unittest

import forza_chat as ch
import forza_friends as fr
import forza_live_lsp as lsp
import forza_live_server as srv

ALL = srv.FEATURE_INVITE_PICK | srv.FEATURE_FRIENDS | srv.FEATURE_CHAT


class FakeTransport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))


class ChatTest(unittest.TestCase):
    def setUp(self):
        self.server = srv.ForzaLive("test", 64)
        self.transport = FakeTransport()
        self.server.transport = self.transport
        self.peers = [self.join(n) for n in range(1, 5)]
        self.transport.sent.clear()

    def join(self, n, features=ALL):
        body = struct.pack(">HQQIH", srv.VERSION, n, 0x0009000000000000 + n, 0, 0)
        tag = b"Driver%d" % n
        body += bytes([4]) + b"#TU4" + bytes([len(tag)]) + tag + bytes([features])
        body += bytes([16]) + bytes(15) + bytes([n])
        addr = ("127.0.0.1", 20000 + n)
        self.server.datagram_received(srv.MAGIC + bytes([srv.HELLO]) + body, addr)
        return self.server.by_addr.get(addr)

    def send(self, peer, body):
        self.server.datagram_received(srv.MAGIC + bytes([srv.CHAT]) + body, peer.addr)

    def sessions(self, peer, *ids):
        self.send(peer, struct.pack(">BB", ch.SESSIONS, len(ids)) +
                  b"".join(struct.pack(">Q", i) for i in ids))

    def say(self, peer, channel, text, target=0):
        self.transport.sent.clear()
        self.send(peer, struct.pack(">BBQ", ch.SAY, channel, target) + ch.text16(text))

    def messages(self, peer):
        out = []
        for addr, d in self.transport.sent:
            if addr == peer.addr and d[4] == srv.CHAT and d[5] == ch.R_MESSAGE:
                channel, xuid, n = struct.unpack_from(">BQB", d, 6)
                name = d[16:16 + n].decode()
                (size,) = struct.unpack_from(">H", d, 16 + n)
                out.append((channel, name, d[18 + n:18 + n + size].decode()))
        return out

    def notices(self, peer):
        return [d[8:].decode() for addr, d in self.transport.sent
                if addr == peer.addr and d[4] == srv.CHAT and d[5] == ch.R_NOTICE]

    def befriend(self, a, b):
        self.server.friends.store.befriend(a.xuid, b.xuid)

    def test_session(self):
        a, b, c, d = self.peers
        self.sessions(a, 0x1111, 0x2222)
        self.sessions(b, 0x2222)
        self.sessions(c, 0x3333)
        self.say(a, ch.SESSION, "hello party")
        self.assertEqual(self.messages(b), [(ch.SESSION, "Driver1", "hello party")])
        self.assertEqual(self.messages(c) + self.messages(d) + self.messages(a), [])
        self.say(d, ch.SESSION, "anyone?")
        self.assertEqual(self.notices(d), ["You are not in a session"])

    def test_world(self):
        a, b, c, _ = self.peers
        sid = bytes.fromhex("00000000000000AA")
        a.match = (lsp.WORLD_HOPPER, sid + bytes(100), 0)
        b.standby = (lsp.WORLD_HOPPER, sid + bytes(100), 0)
        c.standby = (lsp.WORLD_HOPPER, bytes.fromhex("00000000000000BB") + bytes(100), 0)
        # A Horizon World session is not a "session" for chat.
        self.sessions(a, 0xAA)
        self.sessions(b, 0xAA)
        self.say(b, ch.WORLD, "nice drift")
        self.assertEqual(self.messages(a), [(ch.WORLD, "Driver2", "nice drift")])
        self.assertEqual(self.messages(c), [])
        self.say(b, ch.SESSION, "x")
        self.assertEqual(self.notices(b), ["You are not in a session"])

    def test_direct_only_to_friends(self):
        a, b, c, _ = self.peers
        self.befriend(a, b)
        self.say(a, ch.DIRECT, "hi B", b.xuid)
        self.assertEqual(self.messages(b), [(ch.DIRECT, "Driver1", "hi B")])
        self.say(a, ch.DIRECT, "hi C", c.xuid)
        self.assertEqual(self.messages(c), [])
        self.assertEqual(self.notices(a), ["You can only message your friends"])
        self.server.drop(b, "left")
        self.say(a, ch.DIRECT, "there?", b.xuid)
        self.assertEqual(self.notices(a), ["Driver2 is offline"])

    def test_text_is_cleaned_and_capped(self):
        a, b = self.peers[:2]
        self.sessions(a, 7)
        self.sessions(b, 7)
        self.say(a, ch.SESSION, "line\none\x00\x1b[31m " + "x" * 300)
        text = self.messages(b)[0][2]
        self.assertEqual(len(text), ch.MAX_CHARS)
        self.assertTrue(text.startswith("line one [31m xxx"))
        self.say(a, ch.SESSION, "  \n ")
        self.assertEqual(self.messages(b), [])

    def test_rate_limit(self):
        a, b = self.peers[:2]
        self.sessions(a, 7)
        self.sessions(b, 7)
        got = 0
        for i in range(8):
            self.say(a, ch.SESSION, "spam %d" % i)
            got += len(self.messages(b))
        self.assertEqual(got, ch.RATE[1])
        self.assertTrue(self.notices(a)[0].startswith("Slow down"))

    def test_blocks_and_mutes(self):
        a, b, c, _ = self.peers
        for p in (a, b, c):
            self.sessions(p, 7)
        self.server.friends.store.block(b.xuid, a.xuid)
        self.say(a, ch.SESSION, "hi")
        self.assertEqual((self.messages(b), len(self.messages(c))), ([], 1))
        self.say(b, ch.SESSION, "hi")  # the blocker doesn't reach the blocked either
        self.assertEqual((self.messages(a), len(self.messages(c))), ([], 1))
        self.transport.sent.clear()
        self.send(c, struct.pack(">BQB", ch.MUTE, a.xuid, 1))
        muted = [d for addr, d in self.transport.sent if addr == c.addr and d[5] == ch.R_MUTED]
        self.assertEqual(muted[-1][6:], struct.pack(">HQ", 1, a.xuid))
        self.say(a, ch.SESSION, "hi again")
        self.assertEqual(self.messages(c), [])
        self.send(c, struct.pack(">BQB", ch.MUTE, a.xuid, 0))
        self.say(a, ch.SESSION, "and again")
        self.assertEqual(len(self.messages(c)), 1)

    def test_admin_mute_and_kick(self):
        a, b = self.peers[:2]
        self.sessions(a, 7)
        self.sessions(b, 7)
        with tempfile.TemporaryDirectory() as tmp:
            self.server.chat.path = os.path.join(tmp, ch.FILE)
            self.assertIsNone(self.server.chat.admin("mute", "Nobody", 10))
            result = self.server.chat.admin("mute", "driver1", 10)
            self.assertEqual(result["players"][0]["gamertag"], "Driver1")
            self.say(a, ch.SESSION, "hi")
            self.assertEqual(self.messages(b), [])
            self.assertIn("muted you", self.notices(a)[0])
            self.server.chat.admin("mute", "Driver1", 0)
            self.say(a, ch.SESSION, "hi")
            self.assertEqual(len(self.messages(b)), 1)
            # Kick: dropped and refused for the minutes given; kept across a restart.
            self.server.chat.admin("kick", "%016X" % b.xuid, 5)
            self.assertNotIn(b.id, self.server.peers)
            self.transport.sent.clear()
            self.assertIsNone(self.join(2))
            self.assertEqual(self.transport.sent[-1][1][4], srv.ERROR)
            again = ch.Chat(self.server, self.server.friends.store, self.server.chat.path)
            again.load()
            self.assertTrue(again.banned(b.xuid))
            self.server.chat.admin("kick", "Driver2", 0)
            self.assertIsNotNone(self.join(2))

    def test_moderation_ring(self):
        a, b = self.peers[:2]
        self.sessions(a, 7)
        self.sessions(b, 7)
        for i in range(ch.RING + 5):
            self.server.chat.buckets.clear()
            self.say(a, ch.SESSION, "m%d" % i)
        ring = self.server.chat.moderation()["messages"]
        self.assertEqual(len(ring), ch.RING)
        self.assertEqual((ring[-1]["text"], ring[-1]["to"], ring[-1]["channel"]),
                         ("m%d" % (ch.RING + 4), ["Driver2"], "session"))

    def test_old_games(self):
        a = self.peers[0]
        old = self.join(9, features=srv.FEATURE_INVITE_PICK | srv.FEATURE_FRIENDS)
        self.sessions(a, 7)
        self.sessions(old, 7)  # ignored: no chat feature
        self.say(a, ch.SESSION, "hi")
        self.assertEqual(self.messages(old), [])
        self.assertEqual(self.notices(a), ["No one else in your session has chat"])



class CleanTextTest(unittest.TestCase):
    def test_format_and_bidi_characters_become_spaces(self):
        self.assertEqual(ch.clean("hi\u202egnp.exe\u200b x\x85y\x07"), "hi gnp.exe x y")

if __name__ == "__main__":
    unittest.main()
