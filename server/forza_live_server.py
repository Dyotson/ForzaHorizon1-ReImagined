#!/usr/bin/env python3
"""Forza Live: a self-hostable Xbox LIVE stand-in for the Forza Horizon PC port.

Adapted from RecompiledHaloReach's Reach Live server (the rendezvous, relay, presence and
session core is game-agnostic). Players point the game at a server with live_server =
"host[:port]" (or FORZA_SERVER=host[:port]); tools/play.sh starts one on this machine when
none is set. Everyone connected to the same server (and room) is on one virtual network
and appears as a LIVE friend, and game traffic flows between them: directly when UDP hole
punching works, through this server when it doesn't. Nothing here talks to Microsoft or
Turn 10 services.

    python3 server/forza_live_server.py [--port 21200] [--http-port 21201]

The wire format is RecompiledHaloReach's (its docs/online_plan.md, section 5). Standard library only.
"""

import argparse
import asyncio
import json
import logging
import os
import random
import signal
import struct
import time
import urllib.parse

import forza_challenge as challenge
import forza_limits as limits
import forza_live_lsp as lsp
import forza_stats as stats
import forza_world as world

MAGIC = b"RLV1"
VERSION = 1

(HELLO, WELCOME, ERROR, BROADCAST, RELAY, FORWARD, PUNCH, PUNCH_ACK, DATA, BYE, LIST, PEERS,
 PRESENCE, INVITE) = range(1, 15)
MATCH_PUBLISH, MATCH_SEARCH, MATCH_RESULTS = 15, 16, 17  # matchmaking sessions
WORLD_EVENT = 18  # server -> a Horizon World's host: end the free roam for an event
CHALLENGE = challenge.KIND  # 19: head-to-head challenges in the world (forza_challenge.py)
STATS, STATS_ROWS = 20, 21  # Xbox LIVE stats written and read, the rows (forza_stats.py)
MAX_MATCH_RECORD = 1300
MATCH_RESULTS_MAX = 25  # sessions answered per search (Forza asks for up to 25)
SESSION_INFO_SIZE = 0x3C  # XSESSION_INFO: session id, host XNADDR, key-exchange key
MAX_PRESENCE_EXTRA = 1024

PEER_TIMEOUT = 30.0  # seconds without a HELLO before a peer is dropped
MAX_PAYLOAD = 1500
# Per-player limit on bytes the server sends for it (relayed datagrams; a broadcast counts
# once per recipient): a two-player match is about 2 KB/s per player, a full lobby of 16
# a few times that. Defaults to 160 KB/s with 640 KB bursts (--rate-limit).
RATE_LIMIT = 160 * 1024
RATE_BURST = 4 * RATE_LIMIT


def set_rate_limit(bytes_per_second):
    global RATE_LIMIT, RATE_BURST
    RATE_LIMIT, RATE_BURST = bytes_per_second, 4 * bytes_per_second
MAX_NAME = 15  # XUSER_NAME_SIZE minus the terminator
MAX_ROOM = 64
MAX_MOTD = 64  # keeps the WELCOME about as large as the HELLO that asks for it
# Limits against abuse from the internet (every datagram is untrusted):
MAX_PEERS_PER_IP = 16  # registered players behind one public address (a LAN, a CGNAT)
PACKETS_PER_IP = (1500, 3000)  # datagrams/s and burst accepted from one address
HELLOS_PER_IP = (2, 20)  # new registrations/s and burst from one address
ERRORS_PER_IP = (2, 10)  # "not registered" replies/s and burst to one address


def take(buckets, key, limit, cost=1.0):
    """Token bucket per key: limit = (tokens per second, burst). False = over the limit."""
    rate, burst = limit
    now = time.monotonic()
    tokens, last = buckets.get(key, (burst, now))
    tokens = min(burst, tokens + (now - last) * rate)
    ok = tokens >= cost
    buckets[key] = (tokens - cost if ok else tokens, now)
    return ok

log = logging.getLogger("forza-live")


class Peer:
    def __init__(self, peer_id, addr, token, xuid, name, room, local):
        self.id = peer_id
        self.addr = addr  # public (ip, port) as this server sees it
        self.token = token
        self.xuid = xuid
        self.name = name
        self.room = room
        self.local = local  # (ip, port) the client reports for its LAN side
        self.proxied = False  # connected through a trusted proxy (addr is the proxy's socket)
        self.seen = time.monotonic()
        self.since = time.time()
        self.relayed_bytes = 0
        # Token bucket for what this player makes the server send (relay, broadcast).
        self.budget = RATE_BURST
        self.budget_time = time.monotonic()
        # Presence, as friends see it: X_ONLINE_FRIENDSTATE flags, the joinable session
        # (an XSESSION_INFO, zeros when none) and a status line.
        self.state = 0
        self.session = bytes(SESSION_INFO_SIZE)
        self.status = ""
        self.extra = b""  # slots and QoS data of the hosted session, opaque to the server


def ip_to_u32(ip):
    a, b, c, d = (int(x) for x in ip.split("."))
    return a << 24 | b << 16 | c << 8 | d


def u32_to_ip(v):
    return f"{v >> 24}.{v >> 16 & 255}.{v >> 8 & 255}.{v & 255}"


class ForzaLive(asyncio.DatagramProtocol):
    def __init__(self, motd, max_peers):
        self.motd = motd.encode()[:MAX_MOTD]
        self.packet_buckets = {}  # ip -> token bucket of datagrams accepted
        self.hello_buckets = {}
        self.error_buckets = {}
        self.per_ip = {}  # ip -> registered peers
        self.max_peers = max_peers
        self.epoch = random.randrange(1, 0x10000)  # changes when the server restarts
        self.peers = {}  # id -> Peer
        self.by_addr = {}  # (ip, port) -> Peer
        self.next_id = random.randrange(1, 0x1000)
        self.transport = None
        self.started = time.time()
        self.relayed_packets = 0
        self.http_port = 0  # TCP port of the title servers, told to clients; 0 = none
        # The same port as a tunnel publishes it (it differs from --http-port): told to
        # players that arrive through a trusted proxy.
        self.proxy_http_port = 0
        self.dropped_packets = 0  # over a player's rate limit
        self.challenges = challenge.Challenges(self.challenge_send, self.world_of_peer,
                                               lambda pid: getattr(self.peers.get(pid), "name", None))

    @staticmethod
    def limit_key(addr):
        """What the per-address limits count: the IP, but for a trusted proxy (one address
        for all the players behind it) the IP and port, i.e. each tunnelled player."""
        return addr if limits.is_trusted_proxy(addr[0]) else addr[0]

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, kind, body=b""):
        self.transport.sendto(MAGIC + bytes([kind]) + body, addr)

    def error(self, addr, code, message):
        text = message.encode()
        self.send(addr, ERROR, struct.pack(">BH", code, len(text)) + text)

    def datagram_received(self, data, addr):
        if len(data) < 5 or data[:4] != MAGIC or len(data) > 2048:
            return
        if not take(self.packet_buckets, self.limit_key(addr), PACKETS_PER_IP):
            self.dropped_packets += 1
            return
        kind, body = data[4], data[5:]
        try:
            if kind == HELLO:
                self.on_hello(body, addr)
                return
            peer = self.by_addr.get(addr)
            if peer is None:
                # Unknown sender (e.g. the server restarted): ask it to register again
                # (rate limited: the sender's address may be forged).
                if take(self.error_buckets, self.limit_key(addr), ERRORS_PER_IP):
                    self.error(addr, 1, "not registered")
                return
            peer.seen = time.monotonic()
            if kind == BROADCAST:
                self.on_broadcast(peer, body)
            elif kind == RELAY:
                self.on_relay(peer, body)
            elif kind == LIST:
                self.on_list(peer, addr)
            elif kind == PRESENCE:
                self.on_presence(peer, body)
            elif kind == INVITE:
                self.on_invite(peer, body)
            elif kind == BYE:
                self.drop(peer, "left")
            elif kind in (MATCH_PUBLISH, MATCH_SEARCH):
                self.on_match(peer, kind, body)
            elif kind == CHALLENGE and body:
                self.challenges.on_message(peer.id, body)
            elif kind == STATS and body:
                self.on_stats(peer, body)
        except Exception as e:  # a malformed datagram must never end the server
            log.debug("bad packet from %s: %r", addr, e)

    def on_hello(self, body, addr):
        version, token, xuid, local_ip, local_port = struct.unpack_from(">HQQIH", body, 0)
        pos = 24
        room_len = body[pos]
        room = body[pos + 1:pos + 1 + room_len].decode()[:MAX_ROOM]
        pos += 1 + room_len
        name_len = body[pos]
        name = body[pos + 1:pos + 1 + name_len].decode()[:MAX_NAME]
        if version != VERSION:
            self.error(addr, 2, f"server speaks protocol {VERSION}, client {version}")
            return
        peer = self.by_addr.get(addr)
        if peer is not None and peer.token != token:
            self.drop(peer, "restarted")  # same address, new game process
            peer = None
        if peer is None:
            proxied = limits.is_trusted_proxy(addr[0])
            key = self.limit_key(addr)
            # A trusted proxy has no per-address player cap (its players are many), but the
            # whole-server cap and the per-tunnelled-player rates stay.
            if len(self.peers) >= self.max_peers or (
                    not proxied and self.per_ip.get(addr[0], 0) >= MAX_PEERS_PER_IP):
                if take(self.error_buckets, key, ERRORS_PER_IP):
                    self.error(addr, 3, "server full")
                return
            if not take(self.hello_buckets, key, HELLOS_PER_IP):
                return
            while self.next_id in self.peers or self.next_id == 0:
                self.next_id = (self.next_id + 1) & 0xFFFFFF
            peer = Peer(self.next_id, addr, token, xuid, name, room, (local_ip, local_port))
            self.next_id = (self.next_id + 1) & 0xFFFFFF
            peer.proxied = proxied
            self.peers[peer.id] = peer
            self.by_addr[addr] = peer
            self.per_ip[addr[0]] = self.per_ip.get(addr[0], 0) + 1
            log.info("join  #%d %r xuid %016X room %r from %s:%d (lan %s:%d)%s", peer.id, name,
                     xuid, room, addr[0], addr[1], u32_to_ip(local_ip), local_port,
                     " via proxy" if proxied else "")
        peer.seen = time.monotonic()
        peer.name, peer.xuid, peer.room, peer.local = name, xuid, room, (local_ip, local_port)
        # Behind a proxy the observed address is the proxy's local socket, no public address
        # of the player: report none (0.0.0.0:0), as in forward_header.
        public = (0, 0) if peer.proxied else (ip_to_u32(addr[0]), addr[1])
        body = struct.pack(">IHIHH", peer.id, self.epoch, *public, len(self.motd))
        http_port = self.proxy_http_port if peer.proxied and self.proxy_http_port else self.http_port
        self.send(addr, WELCOME, body + self.motd + struct.pack(">H", http_port))

    def forward_header(self, src):
        """Who a forwarded datagram is from, with the addresses other players punch to. A
        player behind a trusted proxy has none (its observed address is the proxy's socket,
        useless to others): clients punch only toward a nonzero public address, so they
        keep using the server relay for it."""
        if src.proxied:
            return struct.pack(">IIHIH", src.id, 0, 0, 0, 0)
        return struct.pack(">IIHIH", src.id, ip_to_u32(src.addr[0]), src.addr[1], src.local[0],
                           src.local[1])

    def room_peers(self, src):
        return [p for p in self.peers.values() if p.room == src.room and p is not src]

    def spend(self, peer, nbytes):
        """Takes nbytes from the player's budget; False (drop it) when over the limit."""
        now = time.monotonic()
        peer.budget = min(RATE_BURST, peer.budget + (now - peer.budget_time) * RATE_LIMIT)
        peer.budget_time = now
        if peer.budget < nbytes:
            self.dropped_packets += 1
            return False
        peer.budget -= nbytes
        return True

    def on_broadcast(self, src, body):
        if len(body) < 4 or len(body) - 4 > MAX_PAYLOAD:
            return
        if not self.spend(src, len(body) * max(1, len(self.room_peers(src)))):
            return
        packet = self.forward_header(src) + body
        for peer in self.room_peers(src):
            self.send(peer.addr, FORWARD, packet)

    def on_relay(self, src, body):
        if len(body) < 8 or len(body) - 8 > MAX_PAYLOAD:
            return
        if not self.spend(src, len(body)):
            return
        (dst_id,) = struct.unpack_from(">I", body, 0)
        dst = self.peers.get(dst_id)
        if dst is None or dst.room != src.room:
            return
        src.relayed_bytes += len(body) - 8
        self.relayed_packets += 1
        self.send(dst.addr, FORWARD, self.forward_header(src) + body[4:])

    def on_presence(self, peer, body):
        """State u32, XSESSION_INFO, status (u8 length + UTF-8), then optional bytes the
        clients define (session slots, QoS data), passed on as they are."""
        (state,) = struct.unpack_from(">I", body, 0)
        session = body[4:4 + SESSION_INFO_SIZE]
        status_len = body[4 + SESSION_INFO_SIZE]
        end = 5 + SESSION_INFO_SIZE + status_len
        status = body[5 + SESSION_INFO_SIZE:end].decode()
        extra = body[end:end + MAX_PRESENCE_EXTRA]
        if len(session) != SESSION_INFO_SIZE:
            return
        if (state, session, status) != (peer.state, peer.session, peer.status):
            log.info("presence #%d %r state %08X session %s %r", peer.id, peer.name, state,
                     session[:8].hex(), status)
        peer.state, peer.session, peer.status, peer.extra = state, session, status, extra

    def on_invite(self, src, body):
        """A game invite: target XUID u64 + the inviter's XSESSION_INFO. Delivered to
        that player in the same room as: inviter id u32, inviter XUID u64, gamertag
        (u8 length + UTF-8), XSESSION_INFO."""
        (target,) = struct.unpack_from(">Q", body, 0)
        session = body[8:8 + SESSION_INFO_SIZE]
        if len(session) != SESSION_INFO_SIZE:
            return
        name = src.name.encode()
        for peer in self.room_peers(src):
            if peer.xuid == target:
                log.info("invite #%d %r -> #%d %r", src.id, src.name, peer.id, peer.name)
                self.send(peer.addr, INVITE, struct.pack(">IQB", src.id, src.xuid, len(name)) +
                          name + session)

    def on_list(self, src, addr):
        """The room's players (the asker first): the friends list of a Live player.
        Each: id u32, XUID u64, gamertag (u8 length + UTF-8), presence state u32,
        XSESSION_INFO, status (u8 length + UTF-8), extra (u16 length + bytes). Sent in
        parts that fit a datagram."""
        peers = ([src] + self.room_peers(src))[:100]
        if not self.spend(src, 600):
            return
        entries = []
        for p in peers:
            name, status = p.name.encode(), p.status.encode()[:255]
            entries.append(struct.pack(">IQB", p.id, p.xuid, len(name)) + name +
                           struct.pack(">I", p.state) + p.session + bytes([len(status)]) + status +
                           struct.pack(">H", len(p.extra)) + p.extra)
        # Part header: total players u16, index of the first entry u16, entries in part u16.
        start = 0
        while True:
            part, size = [], 6
            while start + len(part) < len(entries) and size + len(entries[start + len(part)]) <= 1200:
                size += len(entries[start + len(part)])
                part.append(entries[start + len(part)])
            self.send(addr, PEERS, struct.pack(">HHH", len(entries), start, len(part)) + b"".join(part))
            start += len(part)
            if start >= len(entries) or not part:
                break

    # --- Matchmaking sessions (RecompiledHaloReach's docs/online_plan.md section 5.4) ---
    # A player hosting a matchmaking session publishes it with a search key (the
    # playlist); players searching with that key get the room's matching sessions. The
    # session record is the client's (XSESSION_INFO, slots, properties, QoS data); the
    # server reads only the slots. Every searching group also hosts a session (Reach), and
    # two groups that find each other both join the other and wait for it forever, so a
    # searcher with a session of its own only sees older sessions: newer groups join
    # older ones.
    #
    # Placement: a search gets only sessions with a free public slot, the fullest first
    # (then the oldest), so players gather in as few sessions as possible. Forza's game
    # searches before it hosts and joins the first session it can reach, which makes the
    # Horizon World playlist one shared world per 8 players (docs/online_vision.md).

    @staticmethod
    def match_slots(record):
        """(filled public slots, free public slots) of a session record."""
        if len(record) < SESSION_INFO_SIZE + 4:
            return 0, 1
        max_public, _, filled_public, _ = record[SESSION_INFO_SIZE:SESSION_INFO_SIZE + 4]
        return filled_public, max_public - filled_public

    # Standby records: a player in a session someone else hosts publishes it with its own
    # address as the host (op 2). Forza's game picks a new host among the remaining
    # players when its host leaves, without telling Xbox LIVE, so once nobody hosts a
    # session id any more, its standby records stand in for it: searchers get the record
    # of the member the game made its host (the client sees the game's host flag and
    # sends op 4 instead of op 2), else of the oldest remaining member, with the number of
    # members as the filled slots.

    def standby_sessions(self, src, key):
        """Advertisable standby sessions for playlist key: [(peer, record, since)]."""
        hosted = {p.match[1][:8] for p in self.peers.values()
                  if p.room == src.room and getattr(p, "match", None)}
        groups = {}
        for p in self.room_peers(src):
            standby = getattr(p, "standby", None)
            if standby and standby[0] == key and standby[1][:8] not in hosted:
                groups.setdefault(standby[1][:8], []).append(p)
        own = getattr(src, "standby", None)
        found = []
        for sid, members in groups.items():
            if own and own[1][:8] == sid:
                continue  # the searcher is in it already
            members.sort(key=lambda p: (not getattr(p, "standby_host", False), p.standby[2]))
            record = bytearray(members[0].standby[1])
            if len(record) >= SESSION_INFO_SIZE + 4:
                record[SESSION_INFO_SIZE + 2] = min(len(members), 255)
            found.append((members[0], bytes(record), members[0].standby[2]))
        return found

    def on_match(self, peer, kind, body):
        if kind == MATCH_PUBLISH and body[0] in (2, 3, 4):
            # op 2: standby (search key u32 + the record), op 4: the same from the player
            # the game made the session's host, op 3: withdraw it
            if body[0] == 3:
                peer.standby = None
                peer.standby_host = False
            else:
                host = body[0] == 4
                if host != getattr(peer, "standby_host", False):
                    log.info("match #%d %r %s the game's host of %s", peer.id, peer.name,
                             "is now" if host else "is no longer", body[5:13].hex())
                peer.standby_host = host
                (key,) = struct.unpack_from(">I", body, 1)
                record = body[5:5 + MAX_MATCH_RECORD]
                old = getattr(peer, "standby", None)
                if not old or old[1][:8] != record[:8]:
                    log.info("match #%d %r is in %s (playlist %d)", peer.id, peer.name,
                             record[:8].hex(), key)
                peer.standby = (key, record, old[2] if old and old[1][:8] == record[:8]
                                else time.monotonic())
            return
        if kind == MATCH_PUBLISH:
            # op u8: 0 withdraw, 1 publish (then search key u32 + the record)
            if body[0] == 0:
                if getattr(peer, "match", None):
                    log.info("match #%d %r withdrew its session", peer.id, peer.name)
                peer.match = None
            else:
                (key,) = struct.unpack_from(">I", body, 1)
                record = body[5:5 + MAX_MATCH_RECORD]
                old = getattr(peer, "match", None)
                since = old[2] if old and old[1][:8] == record[:8] else time.monotonic()
                if (not old or old[0] != key
                        or old[1][:SESSION_INFO_SIZE + 4] != record[:SESSION_INFO_SIZE + 4]):
                    filled, free = self.match_slots(record)
                    log.info("match #%d %r hosts %s for playlist %d (%d/%d)", peer.id, peer.name,
                             record[:8].hex(), key, filled, filled + free)
                peer.match = (key, record, since)  # since: when this session first appeared
            return
        # search key u32, request number u32. Reply: request number u32, count u16, then
        # per session the host's id u32, XUID u64 and record (u16 length + bytes), in the
        # order below. A record is about 900 bytes (most of it the lobby's QoS data, which
        # Find Custom Game shows), so the sessions go in as many datagrams as they need, up
        # to MATCH_RESULTS_MAX: bit 15 of the count says that another datagram follows
        # (#61: with one datagram, a second public lobby of a game type was never listed).
        # The client answers the game once the last datagram is in.
        key, request = struct.unpack_from(">II", body, 0)
        own = getattr(peer, "match", None)
        found = [(p, p.match[1], p.match[2]) for p in self.room_peers(peer)
                 if getattr(p, "match", None) and p.match[0] == key
                 and not (own and own[0] == key and p.match[2] >= own[2])]
        found += self.standby_sessions(peer, key)
        found = [f for f in found if self.match_slots(f[1])[1] > 0]
        found.sort(key=lambda f: (-self.match_slots(f[1])[0], f[2]))
        if found:
            filled, free = self.match_slots(found[0][1])
            log.info("match #%d %r playlist %d: %d open session(s), first %s from #%d %r (%d/%d)",
                     peer.id, peer.name, key, len(found), found[0][1][:8].hex(), found[0][0].id,
                     found[0][0].name, filled, filled + free)
        else:
            log.info("match #%d %r playlist %d: no open session, it hosts", peer.id, peer.name, key)
        datagrams, entries, size = [], [], 6
        for p, record, _ in found[:MATCH_RESULTS_MAX]:
            entry = struct.pack(">IQH", p.id, p.xuid, len(record)) + record
            if entries and size + len(entry) > 1400:
                datagrams.append(entries)
                entries, size = [], 6
            entries.append(entry)
            size += len(entry)
        datagrams.append(entries)
        for i, entries in enumerate(datagrams):
            more = 0x8000 if i + 1 < len(datagrams) else 0
            body = struct.pack(">IH", request, more | len(entries)) + b"".join(entries)
            if not self.spend(peer, len(body)):
                break
            self.send(peer.addr, MATCH_RESULTS, body)

    def on_stats(self, peer, body):
        """Xbox LIVE stats (forza_stats.py): op 1 a write (for the player or a player in its
        room: a game's host writes everyone's result), op 2 a read answered with STATS_ROWS."""
        if body[0] == 1:
            room = {p.xuid: p.name for p in self.room_peers(peer)}
            room[peer.xuid] = peer.name
            stats.write(peer.xuid, lambda x: room.get(x) or lsp.gamertag_of(x), body[1:],
                        allowed=set(room))
        elif body[0] == 2:
            names = {p.xuid: p.name for p in self.peers.values()}
            reply = stats.read(peer.xuid, body[1:], lambda x: names.get(x) or lsp.gamertag_of(x))
            if self.spend(peer, len(reply)):
                self.send(peer.addr, STATS_ROWS, reply)

    # --- Horizon World events (forza_world.py) ---

    def world_sessions(self):
        """The Horizon World sessions: {session id: (its host peer, humans)}. A session
        whose host left is led by its standby host (see standby_sessions)."""
        key = lsp.WORLD_HOPPER
        worlds = {}
        for p in self.peers.values():
            match = getattr(p, "match", None)
            if match and match[0] == key:
                worlds[match[1][:8]] = (p, self.match_slots(match[1])[0])
        members = {}
        for p in self.peers.values():
            standby = getattr(p, "standby", None)
            if standby and standby[0] == key and standby[1][:8] not in worlds:
                members.setdefault(standby[1][:8], []).append(p)
        for sid, group in members.items():
            group.sort(key=lambda p: (not getattr(p, "standby_host", False), p.standby[2]))
            worlds[sid] = (group[0], len(group))
        return worlds

    def world_of(self, xuid):
        """(session id, humans) of the world the player with this XUID hosts, or None."""
        for sid, (host, humans) in self.world_sessions().items():
            if host.xuid == xuid:
                return sid, humans
        return None

    def world_tick(self):
        """Every 2 s: tell each world's host about an event waiting for it (op u8 1,
        event id u32); the client ends the free roam once the event is its next round."""
        worlds = self.world_sessions()
        for sid, event in world.WORLDS.tick(set(worlds)):
            host = worlds[sid][0]
            self.send(host.addr, WORLD_EVENT, struct.pack(">BI", 1, event[0]) + sid)

    def world_request(self, session, event_id):
        """GET /world/event?session=<id hex>|all[&event=<id>]: start an event in a world."""
        worlds = self.world_sessions()
        sids = list(worlds) if session in ("", "all") else \
            [sid for sid in worlds if sid.hex() == session.lower()]
        return {"requested": [{"session": sid.hex(), "host": worlds[sid][0].name,
                               "humans": worlds[sid][1],
                               "event": world.WORLDS.request(sid, event_id)[0]} for sid in sids],
                "worlds": [sid.hex() for sid in worlds]}

    # --- head-to-head challenges (forza_challenge.py) ---

    def challenge_send(self, peer_id, body):
        peer = self.peers.get(peer_id)
        if peer is not None:
            self.send(peer.addr, CHALLENGE, body)

    def world_of_peer(self, peer_id):
        """The Horizon World session id the player is in (its hosted or joined session), or
        None."""
        p = self.peers.get(peer_id)
        if p is None:
            return None
        for record in (getattr(p, "match", None), getattr(p, "standby", None)):
            if record and record[0] == lsp.WORLD_HOPPER:
                return bytes(record[1][:8])
        return None

    def drop(self, peer, why):
        self.challenges.drop(peer.id)
        self.peers.pop(peer.id, None)
        if self.by_addr.get(peer.addr) is peer:
            del self.by_addr[peer.addr]
            left = self.per_ip.get(peer.addr[0], 1) - 1
            if left > 0:
                self.per_ip[peer.addr[0]] = left
            else:
                self.per_ip.pop(peer.addr[0], None)
        log.info("leave #%d %r (%s)", peer.id, peer.name, why)

    def expire(self):
        now = time.monotonic()
        for peer in [p for p in self.peers.values() if now - p.seen > PEER_TIMEOUT]:
            self.drop(peer, "timed out")
        for buckets in (self.packet_buckets, self.hello_buckets, self.error_buckets):
            if len(buckets) > 5000:  # forgotten addresses (forged sources)
                for ip in [ip for ip, (_, last) in buckets.items() if now - last > 60]:
                    del buckets[ip]

    def status(self):
        rooms = {}
        for p in self.peers.values():
            rooms.setdefault(p.room, []).append(
                {"id": p.id, "gamertag": p.name, "xuid": f"{p.xuid:016X}",
                 "online_for": int(time.time() - p.since), "relayed_bytes": p.relayed_bytes,
                 "via_proxy": p.proxied,
                 "state": f"{p.state:08X}", "session": p.session[:8].hex(), "status": p.status,
                 "match": None if not getattr(p, "match", None) else
                 {"playlist": p.match[0], "session": p.match[1][:8].hex(),
                  "players": self.match_slots(p.match[1])[0],
                  "slots": sum(self.match_slots(p.match[1]))}})
        worlds = {sid.hex(): {"host": host.name, "humans": humans,
                              "event": world.WORLDS.state.get(sid)}
                  for sid, (host, humans) in self.world_sessions().items()}
        return {"server": "forza-live", "protocol": VERSION, "uptime": int(time.time() - self.started),
                "players": len(self.peers),
                "via_proxy": sum(1 for p in self.peers.values() if p.proxied),
                "relayed_packets": self.relayed_packets,
                "dropped_packets": self.dropped_packets, "rooms": rooms, "worlds": worlds,
                **self.challenges.status()}


# How long a kept-alive connection may sit idle between requests. The game opens one
# connection to its title server and keeps it for every later call; when we closed it
# after 15 idle seconds the game's next call (e.g. Rivals from an event, minutes into the
# single-player free roam) was never sent and timed out (0x80500010, "The Forza Horizon
# server is not available") until the game found its title server again minutes later.
KEEP_ALIVE_IDLE = 6 * 3600


async def read_request(reader, idle=15):
    """One HTTP/1.x request: (method, path, headers, body). Waits up to `idle` seconds
    for the request to start."""
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), idle)
    lines = head.decode("latin-1").split("\r\n")
    method, path = lines[0].split(" ")[:2]
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    if len(lines) > MAX_HEADERS:
        raise ValueError("too many headers")
    length = int(headers.get("content-length", 0) or 0)
    if not 0 <= length <= MAX_BODY:
        raise ValueError("body of %d bytes refused" % length)
    body = await asyncio.wait_for(reader.readexactly(length), 30) if length else b""
    return method, path, headers, body


MAX_BODY = 8388608  # bytes of one request body (uploads); larger requests are refused
MAX_HEADERS = 64
HTTP_PER_IP = 32  # simultaneous connections from one address
HTTP_TOTAL = 1024
http_conns = {}  # ip -> open connections


def response(status, body, content_type="application/octet-stream", extra=()):
    reason = {200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
              500: "Internal Server Error"}
    head = [f"HTTP/1.1 {status} {reason.get(status, 'OK')}", f"Content-Type: {content_type}",
            f"Content-Length: {len(body)}", "Access-Control-Allow-Origin: *", *extra]
    return ("\r\n".join(head) + "\r\n\r\n").encode() + body


async def http_handler(server, reader, writer):
    """The HTTP side of the server, on --http-port. The game reaches it as its title
    servers ("LSP"; forza_live_lsp.py) and keeps the connection open between calls
    (Connection: Keep-Alive). GET / or /status returns a JSON status page for people and
    web front ends."""
    peer = writer.get_extra_info("peername") or ("?", 0)
    ip = peer[0]
    # A trusted proxy carries all its players' connections: no per-address cap for it.
    if ((http_conns.get(ip, 0) >= HTTP_PER_IP and not limits.is_trusted_proxy(ip))
            or sum(http_conns.values()) >= HTTP_TOTAL):
        writer.close()
        return
    http_conns[ip] = http_conns.get(ip, 0) + 1
    try:
        idle = 15  # the first request
        while True:
            method, path, headers, body = await read_request(reader, idle)
            idle = KEEP_ALIVE_IDLE
            keep = headers.get("connection", "").lower() == "keep-alive"
            conn = ("Connection: Keep-Alive" if keep else "Connection: close",)
            if path in ("/", "/status"):
                reply = response(200, json.dumps(server.status(), indent=2, default=str).encode(),
                                 "application/json", conn)
            elif path.startswith("/world/event"):
                query = urllib.parse.parse_qs(urllib.parse.urlsplit(path).query)
                if not lsp.is_admin(peer, headers, query):
                    reply = response(403, b"forbidden", "text/plain", conn)
                    writer.write(reply)
                    await writer.drain()
                    if not keep:
                        break
                    continue
                event = int(query.get("event", ["0"])[0] or 0)
                result = server.world_request(query.get("session", ["all"])[0], event)
                reply = response(200, json.dumps(result, indent=2).encode(), "application/json",
                                 conn)
            else:
                try:
                    status, content_type, data = lsp.handle(server, method, path, headers, body,
                                                            peer)
                except Exception:  # a bad request must not take the server down
                    log.exception("LSP %s %s failed", method, path)
                    status, content_type, data = 500, "text/plain", b"error"
                log.info("http  %s %s %s -> %d (%d bytes)", peer[0], method, path, status,
                         len(data))
                reply = response(status, data, content_type, conn)
            writer.write(reply)
            await writer.drain()
            if not keep:
                break
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, ValueError,
            asyncio.LimitOverrunError, UnicodeError, OSError):
        pass
    except Exception:  # never let one connection's task fail loudly or take the server down
        log.exception("http connection from %s failed", peer)
    finally:
        http_conns[ip] = http_conns.get(ip, 1) - 1
        if http_conns[ip] <= 0:
            http_conns.pop(ip, None)
        writer.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on")
    parser.add_argument("--port", type=int, default=int(os.environ.get("FORZA_LIVE_PORT", 21200)),
                        help="UDP port for the game (default 21200; Reach Live uses 21100)")
    parser.add_argument("--http-port", type=int, default=0,
                        help="TCP port for the game's title servers (LSP) and a JSON status "
                             "page (default: off)")
    parser.add_argument("--motd", default="Welcome to Forza Live", help="message sent to clients")
    parser.add_argument("--max-peers", type=int, default=1024)
    parser.add_argument("--rate-limit", type=int, default=RATE_LIMIT,
                        help="bytes per second the server relays for one player (bursts of 4 s)")
    parser.add_argument("--trusted-proxy", action="append", default=[], metavar="IP[/BITS]",
                        help="address of a tunnel agent (playit.gg) relaying the players: exempt "
                             "from the per-address limits, its players use the server relay "
                             "instead of hole punching (repeatable; env FORZA_TRUSTED_PROXIES, "
                             "comma separated)")
    parser.add_argument("--proxy-http-port", type=int,
                        default=int(os.environ.get("FORZA_PROXY_HTTP_PORT", 0)),
                        help="the TCP port a tunnel publishes --http-port on, told to players "
                             "arriving through a trusted proxy (default: the same port; env "
                             "FORZA_PROXY_HTTP_PORT)")
    parser.add_argument("-v", "--verbose", action="store_true")
    lsp.add_arguments(parser)
    world.add_arguments(parser)
    challenge.add_arguments(parser)
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s")
    limits.configure_proxies(os.environ.get("FORZA_TRUSTED_PROXIES", ""), *args.trusted_proxy)
    if limits.TRUSTED_PROXIES:
        log.info("trusted proxies: %s", ", ".join(map(str, limits.TRUSTED_PROXIES)))
    lsp.configure(args)
    world.configure(args)
    challenge.configure(args)
    set_rate_limit(args.rate_limit)

    loop = asyncio.get_running_loop()
    transport, server = await loop.create_datagram_endpoint(
        lambda: ForzaLive(args.motd, args.max_peers), local_addr=(args.host, args.port))
    log.info("Forza Live listening on UDP %s:%d (epoch %04X)", args.host, args.port, server.epoch)
    if args.http_port:
        server.http_port = args.http_port
        server.proxy_http_port = args.proxy_http_port
        await asyncio.start_server(lambda r, w: http_handler(server, r, w), args.host, args.http_port,
                                 limit=1 << 15)
        log.info("title servers and status page on http://%s:%d/", args.host, args.http_port)
    try:
        ticks = 0
        while True:
            await asyncio.sleep(1)
            ticks += 1
            server.challenges.tick()
            if ticks % 2 == 0:
                server.world_tick()
                stats.settle()
            if ticks % 5 == 0:
                server.expire()
    finally:
        transport.close()


if __name__ == "__main__":
    # SIGTERM (docker stop) ends the server like Ctrl+C; as a container's first process
    # it would otherwise be ignored.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
