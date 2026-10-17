"""A server restart is invisible to the games in a lobby or a Horizon World (issue #93): the
registrations are written to the data folder and loaded again, so a game's next HELLO or
packet with its old token is accepted under the SAME player id. Drives the datagram handler
directly (no sockets)."""

import json
import os
import struct
import tempfile
import time
import unittest

import forza_live_lsp as lsp
import forza_live_server as srv

KEY = lsp.WORLD_HOPPER


class FakeTransport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))

    def kinds(self, addr):
        return [d[4] for a, d in self.sent if a == addr]


def hello(token, xuid, name, room="#TU4"):
    room, name = room.encode(), name.encode()
    body = struct.pack(">HQQIH", srv.VERSION, token, xuid, 0, 0)
    body += bytes([len(room)]) + room + bytes([len(name)]) + name + bytes([1])
    return srv.MAGIC + bytes([srv.HELLO]) + body


def packet(kind, body=b""):
    return srv.MAGIC + bytes([kind]) + body


def record(session_id, filled=1, slots=8):
    info = bytearray(srv.SESSION_INFO_SIZE)
    info[:8] = session_id
    return bytes(info) + bytes([slots, 0, filled, 0]) + bytes(32)


def welcome_id(transport, addr):
    for a, d in reversed(transport.sent):
        if a == addr and d[4] == srv.WELCOME:
            return struct.unpack_from(">IH", d, 5)
    return None


class RestartTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, srv.STATE_FILE)
        self.addr = {n: ("127.0.0.1", 30000 + i) for i, n in enumerate("ABC")}
        self.token = {"A": 0x1111, "B": 0x2222, "C": 0x3333}
        self.xuid = {"A": 0x0009000000000001, "B": 0x0009000000000002, "C": 0x0009000000000003}
        self.server = self.new_server(load=False)

    def tearDown(self):
        self.tmp.cleanup()

    def new_server(self, load=True):
        server = srv.ForzaLive("test", 64)
        server.transport = FakeTransport()
        server.state_path = self.path
        if load:
            server.load_state()
        return server

    def read_state(self):
        with open(self.path) as f:
            return json.load(f)

    def join(self, server, who, room="#TU4"):
        server.datagram_received(hello(self.token[who], self.xuid[who], "player " + who, room),
                                 self.addr[who])
        return welcome_id(server.transport, self.addr[who])

    def restart(self):
        self.server.save_state()
        self.server = self.new_server()
        return self.server

    def test_same_ids_after_restart(self):
        a, b = self.join(self.server, "A"), self.join(self.server, "B")
        self.assertNotEqual(a[0], b[0])
        server = self.restart()
        self.assertEqual(sorted(server.peers), sorted([a[0], b[0]]))
        self.assertEqual(server.epoch, a[1])
        self.assertEqual(self.join(server, "A"), a)  # the WELCOME says the same id and epoch
        self.assertEqual(self.join(server, "B"), b)
        self.assertEqual(len(server.peers), 2)
        self.assertEqual(server.restored, set())
        # a new game gets a new id
        c = self.join(server, "C")
        self.assertNotIn(c[0], (a[0], b[0]))

    def test_relay_continues_without_hello(self):
        """The games' next datagram after the restart is accepted, not 'not registered'."""
        a, b = self.join(self.server, "A")[0], self.join(self.server, "B")[0]
        server = self.restart()
        server.datagram_received(packet(srv.RELAY, struct.pack(">I", a) + b"\0\0\0\0hi"),
                                 self.addr["B"])
        sent = server.transport.sent
        self.assertEqual([(addr, d[4]) for addr, d in sent], [(self.addr["A"], srv.FORWARD)])
        self.assertEqual(struct.unpack_from(">I", sent[0][1], 5)[0], b)  # from B's id
        server.datagram_received(packet(srv.BROADCAST, b"\0\0\0\0hello"), self.addr["A"])
        self.assertEqual(server.transport.kinds(self.addr["B"]), [srv.FORWARD])

    def test_unknown_sender_still_told_to_register(self):
        self.join(self.server, "A")  # never saved: a restart without the state file
        server = self.new_server()
        server.datagram_received(packet(srv.LIST), self.addr["A"])
        self.assertEqual(server.transport.kinds(self.addr["A"]), [srv.ERROR])

    def test_presence_and_rooms_survive(self):
        self.join(self.server, "A", room="#TU4"), self.join(self.server, "B", room="#TU4")
        self.join(self.server, "C", room="#other")
        presence = struct.pack(">I", 0x11) + bytes(range(srv.SESSION_INFO_SIZE)) + \
            bytes([2]) + b"ok" + b"extra!"
        self.server.datagram_received(packet(srv.PRESENCE, presence), self.addr["A"])
        before = {i: (p.room, p.xuid, p.name, p.state, p.session, p.status, p.extra, p.features,
                      p.token) for i, p in self.server.peers.items()}
        server = self.restart()
        after = {i: (p.room, p.xuid, p.name, p.state, p.session, p.status, p.extra, p.features,
                     p.token) for i, p in server.peers.items()}
        self.assertEqual(before, after)
        self.assertEqual(len(server.room_peers(next(p for p in server.peers.values()
                                                    if p.name == "player A"))), 1)

    def test_world_keeps_one_host(self):
        """Host A and joiner B of one Horizon World, and C alone in another: after the
        restart the same two worlds, with the same hosts, B is not a host of its own."""
        for who in "ABC":
            self.join(self.server, who)
        sid1, sid2 = b"WORLD001", b"WORLD002"
        pub = lambda sid: bytes([1]) + struct.pack(">I", KEY) + record(sid, 2)
        stand = lambda sid, op: bytes([op]) + struct.pack(">I", KEY) + record(sid, 2)
        self.server.datagram_received(packet(srv.MATCH_PUBLISH, pub(sid1)), self.addr["A"])
        self.server.datagram_received(packet(srv.MATCH_PUBLISH, stand(sid1, 2)), self.addr["B"])
        self.server.datagram_received(packet(srv.MATCH_PUBLISH, pub(sid2)), self.addr["C"])
        before = {sid: (host.id, humans) for sid, (host, humans) in
                  self.server.world_sessions().items()}
        self.assertEqual(sorted(before), [sid1, sid2])
        server = self.restart()
        after = {sid: (host.id, humans) for sid, (host, humans) in server.world_sessions().items()}
        self.assertEqual(before, after)
        # the games re-send their records and HELLOs: still the same two worlds
        for who in "ABC":
            self.join(server, who)
        server.datagram_received(packet(srv.MATCH_PUBLISH, pub(sid1)), self.addr["A"])
        server.datagram_received(packet(srv.MATCH_PUBLISH, stand(sid1, 2)), self.addr["B"])
        self.assertEqual({sid: (h.id, n) for sid, (h, n) in server.world_sessions().items()},
                         before)
        # C hosts its own world and sees no standby copy of the others
        found = server.standby_sessions(server.by_addr[self.addr["C"]], KEY)
        self.assertEqual([m.name for m, _, _ in found], [])  # C hosts world 2, A hosts world 1

    def test_standby_host_after_restart(self):
        """The host left before the restart: the restored standby members still lead."""
        for who in "AB":
            self.join(self.server, who)
        sid = b"WORLD001"
        stand = lambda op: bytes([op]) + struct.pack(">I", KEY) + record(sid, 2)
        self.server.datagram_received(packet(srv.MATCH_PUBLISH, stand(2)), self.addr["A"])
        self.server.datagram_received(packet(srv.MATCH_PUBLISH, stand(4)), self.addr["B"])
        server = self.restart()
        (host, humans), = server.world_sessions().values()
        self.assertEqual((host.name, humans), ("player B", 2))

    def test_old_state_ignored(self):
        self.join(self.server, "A")
        self.server.save_state()
        state = self.read_state()
        state["saved_at"] -= srv.STATE_MAX_AGE + 60
        with open(self.path, "w") as f:
            json.dump(state, f)
        server = self.new_server()
        self.assertEqual(server.peers, {})

    def test_bad_state_ignored(self):
        for junk in ("", "{", "[]", '{"version": 1}', '{"version": 99, "saved_at": %f}' % time.time()):
            with open(self.path, "w") as f:
                f.write(junk)
            self.assertEqual(self.new_server().peers, {}, junk)

    def test_ids_not_reused(self):
        ids = [self.join(self.server, w)[0] for w in "AB"]
        server = self.restart()
        for peer in list(server.peers.values()):
            server.drop(peer, "test")
        c = self.join(server, "C")[0]
        self.assertNotIn(c, ids)

    def test_restored_peer_times_out(self):
        self.join(self.server, "A")
        server = self.restart()
        server.peers[next(iter(server.peers))].seen -= srv.PEER_TIMEOUT + 1
        server.expire()
        self.assertEqual(server.peers, {})
        self.assertEqual(server.per_ip, {})

    def test_new_game_process_same_address_gets_new_id(self):
        a = self.join(self.server, "A")[0]
        server = self.restart()
        self.token["A"] = 0x9999
        self.assertNotEqual(self.join(server, "A")[0], a)
        self.assertEqual(len(server.peers), 1)

    def test_rebind_when_address_changed(self):
        a = self.join(self.server, "A")
        server = self.restart()
        old = self.addr["A"]
        self.addr["A"] = ("127.0.0.1", 40000)  # the NAT gave another port during the restart
        self.assertEqual(self.join(server, "A"), a)
        self.assertEqual(len(server.peers), 1)
        self.assertNotIn(old, server.by_addr)
        self.assertEqual(server.per_ip, {"127.0.0.1": 1})

    def test_no_takeover_of_live_registration(self):
        a = self.join(self.server, "A")[0]  # not restored: a registered, live player
        self.addr["A"] = ("127.0.0.1", 40000)
        self.assertNotEqual(self.join(self.server, "A")[0], a)  # a new registration
        self.assertEqual(len(self.server.peers), 2)

    def test_rebind_needs_the_token_and_xuid(self):
        a = self.join(self.server, "A")[0]
        server = self.restart()
        self.addr["A"] = ("127.0.0.1", 40001)
        self.xuid["A"] += 1  # same token, other player
        self.assertNotEqual(self.join(server, "A")[0], a)

    def test_saved_on_change(self):
        self.join(self.server, "A")
        self.assertTrue(self.server.state_dirty)
        self.server.maybe_save_state()
        self.assertFalse(self.server.state_dirty)
        self.assertEqual(len(self.read_state()["peers"]), 1)
        self.server.datagram_received(packet(srv.BYE), self.addr["A"])
        self.assertTrue(self.server.state_dirty)
        self.server.state_saved -= 3
        self.server.maybe_save_state()
        self.assertEqual(self.read_state()["peers"], [])

    def test_unchanged_hello_does_not_dirty(self):
        self.join(self.server, "A")
        self.server.save_state()
        self.join(self.server, "A")
        self.server.datagram_received(packet(srv.LIST), self.addr["A"])
        self.assertFalse(self.server.state_dirty)

    def test_unwritable_state_does_not_stop_the_server(self):
        self.server.state_path = os.path.join(self.tmp.name, "missing", "state.json")
        self.join(self.server, "A")
        self.assertFalse(self.server.save_state())


if __name__ == "__main__":
    unittest.main()
