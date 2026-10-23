"""Behind a tunnel agent (playit.gg): --trusted-proxy exempts its address from the
per-address limits and keeps its players on the server relay."""

import struct
import unittest

import forza_limits as limits
import forza_live_lsp as lsp
import forza_live_server as srv

PROXY = "192.168.100.105"


class FakeTransport:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((addr, data))

    def kinds(self, addr):
        return [d[4] for a, d in self.sent if a == addr]


def hello(token, name, room="r"):
    body = struct.pack(">HQQIH", srv.VERSION, token, 0x0009000000000000 | token, 0, 0)
    body += bytes([len(room)]) + room.encode() + bytes([len(name)]) + name.encode()
    return srv.MAGIC + bytes([srv.HELLO]) + body


class ProxyTest(unittest.TestCase):
    def setUp(self):
        limits.configure_proxies(PROXY + ", 127.0.0.1")
        self.server = srv.ForzaLive("hi", 1024)
        self.net = FakeTransport()
        self.server.connection_made(self.net)

    def tearDown(self):
        limits.configure_proxies("")

    def join(self, addr, n):
        self.server.datagram_received(hello(n, f"P{n}"), addr)
        return self.server.by_addr.get(addr)

    def test_parse(self):
        self.assertEqual(len(limits.parse_proxies("10.0.0.0/8 1.2.3.4", "::1,127.0.0.1")), 4)
        limits.configure_proxies("10.0.0.0/8")
        self.assertTrue(limits.is_trusted_proxy("10.9.9.9"))
        self.assertFalse(limits.is_trusted_proxy("11.0.0.1"))
        self.assertFalse(limits.is_trusted_proxy("not an ip"))

    def test_title_server_port_told_to_tunnelled_players(self):
        self.server.http_port, self.server.proxy_http_port = 21201, 56678
        for addr, want in (((PROXY, 40000), 56678), (("203.0.113.7", 40001), 21201)):
            self.net.sent.clear()
            self.server.datagram_received(hello(addr[1], "P"), addr)
            welcome = [d for a, d in self.net.sent if a == addr and d[4] == srv.WELCOME][0]
            self.assertEqual(struct.unpack(">H", welcome[-2:])[0], want)
        self.server.proxy_http_port = 0  # unset: everyone gets --http-port
        self.net.sent.clear()
        self.server.datagram_received(hello(7, "Q"), (PROXY, 40002))
        self.assertEqual(struct.unpack(">H", self.net.sent[-1][1][-2:])[0], 21201)

    def test_many_players_from_one_proxy_address(self):
        peers = [self.join((PROXY, 40000 + i), i + 1) for i in range(40)]
        self.assertTrue(all(p is not None and p.proxied for p in peers))
        self.assertEqual(len(self.server.peers), 40)
        self.assertEqual(self.server.status()["players"], 40)
        self.assertEqual(self.server.status()["via_proxy"], 40)

    def test_global_max_peers_still_applies(self):
        self.server.max_peers = 5
        for i in range(8):
            self.join((PROXY, 40000 + i), i + 1)
        self.assertEqual(len(self.server.peers), 5)

    def test_untrusted_address_still_capped(self):
        peers = [self.join(("203.0.113.7", 40000 + i), i + 1) for i in range(srv.MAX_PEERS_PER_IP + 4)]
        self.assertEqual(len([p for p in peers if p]), srv.MAX_PEERS_PER_IP)
        self.assertFalse(any(p.proxied for p in peers if p))

    def test_hello_rate_still_applies_per_tunnelled_player(self):
        # one tunnelled player (one source port) cannot flood new registrations
        addr = (PROXY, 41000)
        for i in range(60):
            self.server.datagram_received(hello(1000 + i, "Flood"), addr)
        self.assertLessEqual(len(self.server.peers), srv.HELLOS_PER_IP[1] + 1)
        # ...and does not starve the others behind the same proxy
        self.assertIsNotNone(self.join((PROXY, 41001), 7))

    def test_hello_rate_untrusted(self):
        for i in range(60):
            self.server.datagram_received(hello(i + 1, "x"), ("198.51.100.%d" % 5, 1000 + i))
        self.assertLessEqual(len(self.server.peers), srv.MAX_PEERS_PER_IP)

    def test_proxy_peers_get_no_punch_address(self):
        a = self.join((PROXY, 40001), 1)
        b = self.join(("203.0.113.9", 5555), 2)  # direct player
        c = self.join(("203.0.113.10", 6666), 3)
        # welcome of a proxied player carries no address
        welcome = [d for ad, d in self.net.sent if ad == a.addr and d[4] == srv.WELCOME][0]
        _id, _epoch, ip, port, _ = struct.unpack_from(">IHIHH", welcome, 5)
        self.assertEqual((ip, port), (0, 0))
        # what others learn about the proxied player: no public and no LAN address
        for peer in (b, c):
            hdr = self.server.forward_header(a)
            self.assertEqual(struct.unpack(">IIHIH", hdr), (a.id, 0, 0, 0, 0))
        # direct players keep their address (punching stays)
        _i, ip, port, lip, lport = struct.unpack(">IIHIH", self.server.forward_header(b))
        self.assertEqual((srv.u32_to_ip(ip), port), ("203.0.113.9", 5555))
        # a relayed datagram from the proxied player and to it
        self.net.sent.clear()
        self.server.datagram_received(
            srv.MAGIC + bytes([srv.RELAY]) + struct.pack(">I", b.id) + b"\x00\x01\x00\x02hi", a.addr)
        (addr, data), = self.net.sent
        self.assertEqual((addr, data[4]), (b.addr, srv.FORWARD))
        self.assertEqual(struct.unpack_from(">IIHIH", data, 5), (a.id, 0, 0, 0, 0))
        self.net.sent.clear()
        self.server.datagram_received(
            srv.MAGIC + bytes([srv.RELAY]) + struct.pack(">I", a.id) + b"\x00\x01\x00\x02yo", b.addr)
        (addr, data), = self.net.sent
        self.assertEqual((addr, data[4]), (a.addr, srv.FORWARD))
        # nobody receives the proxy's address as a punch target
        self.assertNotIn(bytes(map(int, PROXY.split("."))), b"".join(d for _, d in self.net.sent))

    def test_proxy_players_still_have_a_relay_budget(self):
        a = self.join((PROXY, 40001), 1)
        self.join(("203.0.113.9", 5555), 2)
        big = srv.MAGIC + bytes([srv.BROADCAST]) + b"\0\0\0\0" + bytes(1400)
        for _ in range(srv.RATE_BURST // len(big) + 50):  # the burst, then over it
            self.server.datagram_received(big, a.addr)
        self.assertGreater(self.server.dropped_packets, 0)

    def test_untrusted_packet_rate_applies_per_player_via_proxy(self):
        a = (PROXY, 40001)
        self.join(a, 1)
        for _ in range(srv.PACKETS_PER_IP[1] + 100):
            self.server.datagram_received(srv.MAGIC + bytes([srv.LIST]), a)
        self.assertGreater(self.server.dropped_packets, 0)
        # another player behind the same proxy is not affected
        before = self.server.dropped_packets
        b = self.join((PROXY, 40002), 2)
        self.server.datagram_received(srv.MAGIC + bytes([srv.LIST]), b.addr)
        self.assertEqual(self.server.dropped_packets, before)

    def test_admin_not_granted_to_proxy_loopback(self):
        old = lsp.ADMIN_TOKEN
        lsp.ADMIN_TOKEN = ""
        try:
            self.assertFalse(lsp.is_admin(("127.0.0.1", 1), {}, {}))  # 127.0.0.1 is the proxy
            limits.configure_proxies(PROXY)
            self.assertTrue(lsp.is_admin(("127.0.0.1", 1), {}, {}))
            lsp.ADMIN_TOKEN = "tok"
            limits.configure_proxies("127.0.0.1")
            self.assertTrue(lsp.is_admin(("127.0.0.1", 1), {"x-admin-token": "tok"}, {}))
        finally:
            lsp.ADMIN_TOKEN = old

    def test_without_option_nothing_changes(self):
        limits.configure_proxies("")
        peers = [self.join((PROXY, 40000 + i), i + 1) for i in range(srv.MAX_PEERS_PER_IP + 3)]
        self.assertEqual(len([p for p in peers if p]), srv.MAX_PEERS_PER_IP)
        self.assertFalse(any(p.proxied for p in peers if p))


if __name__ == "__main__":
    unittest.main()
