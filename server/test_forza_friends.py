"""Friends (#92, forza_friends.py): requests, accept, decline, remove, block, caps, rate
limits, the friends key, the list in parts, search, the file, and old games left alone.
Drives the server's handlers directly (no sockets)."""

import os
import struct
import tempfile
import unittest

import forza_friends as fr
import forza_live_server as srv

KEY = bytes(range(16))


def s8(text):
    return fr.text8(text)


class FakeTransport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))


class FriendsTest(unittest.TestCase):
    def setUp(self):
        self.server = srv.ForzaLive("test", 64)
        self.transport = FakeTransport()
        self.server.transport = self.transport
        self.peers = [self.join(n) for n in range(1, 5)]
        self.transport.sent.clear()

    def join(self, n, features=srv.FEATURE_INVITE_PICK | srv.FEATURE_FRIENDS, key=None,
             xuid=None, name=None):
        body = struct.pack(">HQQIH", srv.VERSION, n, xuid or 0x0009000000000000 + n, 0, 0)
        tag = (name or "Driver%d" % n).encode()
        body += bytes([4]) + b"#TU4" + bytes([len(tag)]) + tag + bytes([features])
        k = KEY[:15] + bytes([n]) if key is None else key
        body += bytes([len(k)]) + k
        addr = ("127.0.0.1", 20000 + n)
        self.server.datagram_received(srv.MAGIC + bytes([srv.HELLO]) + body, addr)
        return self.server.by_addr[addr]

    def op(self, peer, body):
        self.server.datagram_received(srv.MAGIC + bytes([srv.FRIEND]) + body, peer.addr)

    def request(self, a, b=None, name=""):
        self.op(a, struct.pack(">BQ", fr.REQUEST, b.xuid if b else 0) + s8(name))

    def act(self, a, op, b):
        self.op(a, struct.pack(">BQ", op, b.xuid) + (s8(b.name) if op == fr.BLOCK else b""))

    def got(self, peer, kind=srv.FRIEND):
        return [d[5:] for addr, d in self.transport.sent if addr == peer.addr and d[4] == kind]

    def notices(self, peer):
        out = []
        for body in self.got(peer):
            if body[0] == fr.R_NOTICE:
                event, xuid = struct.unpack_from(">BQ", body, 1)
                name, pos = fr.read8(body, 10)
                text, _ = fr.read8(body, pos)
                out.append((event, xuid, name, text))
        return out

    def rel(self, a, b):
        return self.server.friends.store.relation(a.xuid, b.xuid)

    def list_of(self, peer, known=0):
        """The entries of the list the server sends (parts joined)."""
        self.transport.sent.clear()
        self.op(peer, struct.pack(">BI", fr.LIST, known))
        rows, version, total = [], None, 0
        for body in self.got(peer):
            if body[0] == fr.R_SAME:
                return "same", struct.unpack_from(">I", body, 1)[0]
            if body[0] != fr.R_LIST:
                continue
            version, total, first, count = struct.unpack_from(">IHHH", body, 1)
            self.assertEqual(first, len(rows))
            pos = 11
            for _ in range(count):
                xuid, rel, state = struct.unpack_from(">QBI", body, pos)
                name, pos = fr.read8(body, pos + 13)
                status, pos = fr.read8(body, pos)
                rows.append((name, rel, state, status))
            self.assertLessEqual(len(body) + 5, 1300)
        self.assertEqual(len(rows), total)
        return rows, version

    def test_request_accept(self):
        a, b, c, _ = self.peers
        self.request(a, name="driver2")  # by gamertag, any case
        self.assertEqual(self.rel(a, b), fr.OUTGOING)
        self.assertEqual(self.rel(b, a), fr.INCOMING)
        self.assertIn((fr.N_REQUEST, a.xuid, "Driver1", ""), self.notices(b))
        self.assertEqual(self.notices(c), [])
        self.transport.sent.clear()
        self.act(b, fr.ACCEPT, a)
        self.assertEqual(self.rel(a, b), fr.FRIEND)
        self.assertEqual(self.rel(b, a), fr.FRIEND)
        self.assertIn((fr.N_ACCEPTED, b.xuid, "Driver2", ""), self.notices(a))
        b.state, b.status = 3, "Free roam"
        rows, _ = self.list_of(a)
        self.assertEqual(rows, [("Driver2", fr.FRIEND, 3, "Free roam")])
        self.assertEqual(self.list_of(c)[0], [])

    def test_request_both_ways_is_a_yes(self):
        a, b = self.peers[:2]
        self.request(a, b)
        self.request(b, a)
        self.assertEqual(self.rel(a, b), fr.FRIEND)

    def test_decline_and_take_back(self):
        a, b, c, _ = self.peers
        self.request(a, b)
        self.act(b, fr.DECLINE, a)
        self.assertEqual((self.rel(a, b), self.rel(b, a)), (0, 0))
        self.request(a, c)
        self.act(a, fr.DECLINE, c)  # takes back the request
        self.assertEqual((self.rel(a, c), self.rel(c, a)), (0, 0))

    def test_remove(self):
        a, b = self.peers[:2]
        self.request(a, b)
        self.act(b, fr.ACCEPT, a)
        self.act(a, fr.REMOVE, b)
        self.assertEqual((self.rel(a, b), self.rel(b, a)), (0, 0))

    def test_block(self):
        a, b = self.peers[:2]
        self.request(a, b)
        self.act(b, fr.ACCEPT, a)
        self.act(b, fr.BLOCK, a)
        self.assertEqual(self.rel(b, a), fr.BLOCKED)
        self.assertEqual(self.rel(a, b), 0)
        self.transport.sent.clear()
        self.request(a, b)  # looks sent, isn't
        self.assertEqual(self.notices(a)[-1][3], "Friend request sent to Driver2")
        self.assertEqual(self.rel(b, a), fr.BLOCKED)
        self.assertEqual(self.notices(b), [])
        # Invites from a blocked player are dropped.
        self.transport.sent.clear()
        self.server.on_invite(a, struct.pack(">Q", b.xuid) + bytes(srv.SESSION_INFO_SIZE))
        self.assertEqual(self.got(b, srv.INVITE), [])
        self.act(b, fr.UNBLOCK, a)
        self.assertEqual(self.rel(b, a), 0)
        self.server.on_invite(a, struct.pack(">Q", b.xuid) + bytes(srv.SESSION_INFO_SIZE))
        self.assertEqual(len(self.got(b, srv.INVITE)), 1)

    def test_unknown_players(self):
        a = self.peers[0]
        self.transport.sent.clear()
        self.request(a, name="Nobody")
        self.assertEqual(self.notices(a)[-1][3], "No player named Nobody")
        self.op(a, struct.pack(">BQ", fr.REQUEST, 0x0009000000ABCDEF) + s8(""))
        self.assertIsNone(self.server.friends.store.get(0x0009000000ABCDEF))
        self.request(a, a)
        self.assertEqual(self.notices(a)[-1][3], "That's you")

    def test_rate_limit(self):
        a = self.peers[0]
        others = [self.join(n) for n in range(10, 22)]
        for p in others:
            self.request(a, p)
        asked = [p for p in others if self.rel(p, a) == fr.INCOMING]
        self.assertEqual(len(asked), fr.REQUESTS_PER_MIN // 2)  # the burst
        self.assertEqual(self.notices(a)[-1][3], "Too many friend requests; wait a minute")

    def test_caps(self):
        a = self.peers[0]
        store = self.server.friends.store
        for n in range(fr.MAX_PENDING):
            store.ask(0x0009000000100000 + n, a.xuid)
        b = self.peers[1]
        self.transport.sent.clear()
        self.request(b, a)
        self.assertEqual(self.rel(a, b), 0)
        self.assertIn("can't take more", self.notices(b)[-1][3])
        for n in range(fr.MAX_FRIENDS):
            store.befriend(0x0009000000200000 + n, b.xuid)
        self.transport.sent.clear()
        self.request(b, self.peers[2])
        self.assertIn("full", self.notices(b)[-1][3])

    def test_the_list_in_parts(self):
        a = self.peers[0]
        store = self.server.friends.store
        for n in range(150):
            x = 0x0009000000300000 + n
            store.seen(x, "Friend%03d" % n)
            store.befriend(a.xuid, x)
        store.ask(0x0009000000400000, a.xuid)
        rows, version = self.list_of(a)
        self.assertEqual(len(rows), 151)
        self.assertEqual(rows[0][1], fr.FRIEND)
        self.assertEqual(rows[-1][1], fr.INCOMING)
        self.assertEqual(self.list_of(a, version), ("same", version))

    def test_online_friends_first(self):
        a, b, c, d = self.peers
        for p in (b, c, d):
            self.request(a, p)
            self.act(p, fr.ACCEPT, a)
        self.server.drop(c, "left")
        rows, _ = self.list_of(a)
        self.assertEqual([r[0] for r in rows], ["Driver2", "Driver4", "Driver3"])
        self.assertEqual(rows[2][2], 0)  # offline

    def test_sign_in_pushes_the_friends_lists(self):
        a, b = self.peers[:2]
        self.request(a, b)
        self.act(b, fr.ACCEPT, a)
        self.server.drop(b, "left")
        self.transport.sent.clear()
        self.join(2)
        self.assertTrue(any(body[0] == fr.R_LIST for body in self.got(a)))

    def test_friends_key(self):
        a, b = self.peers[:2]
        self.request(a, b)
        # Another installation with Driver2's XUID and another key: no list, no changes.
        fake = self.join(9, xuid=b.xuid, name="Driver2", key=b"\xEE" * 16)
        self.assertFalse(fake.friends_ok)
        self.transport.sent.clear()
        self.act(fake, fr.ACCEPT, a)
        self.assertEqual(self.rel(b, a), fr.INCOMING)
        self.assertIn("another installation", self.notices(fake)[-1][3])
        self.assertEqual([body for body in self.got(fake) if body[0] == fr.R_LIST], [])
        self.assertEqual(self.list_of(fake), ([], None))
        self.assertTrue(b.friends_ok)

    def test_old_games(self):
        a = self.peers[0]
        old = self.join(7, features=srv.FEATURE_INVITE_PICK, key=b"")
        self.transport.sent.clear()
        self.request(a, old)  # kept for when it updates
        self.assertEqual(self.rel(old, a), fr.INCOMING)
        self.assertEqual(self.got(old), [])
        # Old games still list everyone in the room.
        self.server.on_list(old, old.addr)
        peers = self.got(old, srv.PEERS)
        self.assertEqual(struct.unpack_from(">H", peers[0])[0], 5)

    def test_search(self):
        a = self.peers[0]
        self.server.friends.store.seen(0x0009000000500000, "OfflineDriver")
        self.transport.sent.clear()
        self.op(a, struct.pack(">BH", fr.SEARCH, 7) + s8("driver"))
        body = self.got(a)[-1]
        request, count = struct.unpack_from(">HB", body, 1)
        pos, names = 4, []
        for _ in range(count):
            xuid, online, rel = struct.unpack_from(">QBB", body, pos)
            name, pos = fr.read8(body, pos + 10)
            names.append((name, online))
        self.assertEqual(request, 7)
        self.assertEqual(names, [("Driver2", 1), ("Driver3", 1), ("Driver4", 1),
                                 ("OfflineDriver", 0)])

    def test_file(self):
        a, b, c = self.peers[:3]
        self.request(a, b)
        self.act(b, fr.ACCEPT, a)
        self.request(c, a)
        self.act(c, fr.BLOCK, b)
        with tempfile.TemporaryDirectory() as tmp:
            store = self.server.friends.store
            store.path = os.path.join(tmp, fr.FILE)
            self.assertTrue(store.save())
            again = fr.Store(store.path)
            again.load()
            self.assertEqual(again.players, store.players)
            self.assertEqual(again.relation(a.xuid, b.xuid), fr.FRIEND)
            self.assertEqual(again.relation(a.xuid, c.xuid), fr.INCOMING)
            self.assertTrue(again.blocks(c.xuid, b.xuid))
            with open(store.path, "w") as f:
                f.write("{broken")
            broken = fr.Store(store.path)
            broken.load()  # logged, not fatal
            self.assertEqual(broken.players, {})

    def test_restart_keeps_the_friends_flag(self):
        a = self.peers[0]
        snap = self.server.snapshot()
        self.assertTrue(next(p for p in snap["peers"] if p["id"] == a.id)["friends_ok"])


if __name__ == "__main__":
    unittest.main()
