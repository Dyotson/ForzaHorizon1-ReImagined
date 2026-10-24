#!/usr/bin/env python3
"""Forza Live: a self-hostable Xbox LIVE stand-in for the Forza Horizon PC port.

Adapted from RecompiledHaloReach's Reach Live server (the rendezvous, relay, presence and
session core is game-agnostic). Players point the game at a server with live_server =
"host[:port]" (or FORZA_SERVER=host[:port]); tools/play.sh starts one on this machine when
none is set. Everyone connected to the same server (and room) is on one virtual network
and sees the others online (games up to v0.1.14 count them all as LIVE friends; newer ones
keep a friends list here, forza_friends.py), and game traffic flows between them: directly
when UDP hole punching works, through this server when it doesn't. Nothing here talks to Microsoft or
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
import sqlite3
import struct
import time
import urllib.parse

import forza_challenge as challenge
import forza_chat as chat
import forza_discord_status as discord_status
import forza_friends as friends
import forza_events as events
import forza_limits as limits
import forza_live_lsp as lsp
import forza_stats as stats
import forza_campaign as campaign
import forza_coop_ledger as coop_ledger
import forza_world as world
import forza_world_host as world_host

MAGIC = b"RLV1"
VERSION = 1

(HELLO, WELCOME, ERROR, BROADCAST, RELAY, FORWARD, PUNCH, PUNCH_ACK, DATA, BYE, LIST, PEERS,
 PRESENCE, INVITE) = range(1, 15)
MATCH_PUBLISH, MATCH_SEARCH, MATCH_RESULTS = 15, 16, 17  # matchmaking sessions
# Invites one player may send per INVITE_WINDOW seconds; the rest are dropped (#90: games up
# to v0.1.12 invited everyone in the room at once, and accepted every invite by themselves).
INVITE_LIMIT, INVITE_WINDOW = 4, 30.0
# HELLO's features byte (after the gamertag; older games send none). FEATURE_INVITE_PICK:
# the game sends invites only to players its player picked and asks before joining one
# (#90). Invites from or to a game without it are dropped.
FEATURE_INVITE_PICK = 0x01
# FEATURE_FRIENDS: the game shows the server's friends list (FRIEND messages,
# forza_friends.py, #92) instead of everyone in the room; its friends key follows the
# features byte (u8 length + bytes).
FEATURE_FRIENDS = friends.FEATURE
FRIEND = friends.KIND  # 22
# FEATURE_CHAT: the game has text chat (CHAT messages, forza_chat.py, #92).
FEATURE_CHAT = chat.FEATURE
CHAT = chat.KIND  # 23
WORLD_EVENT = 18  # server -> a Horizon World's host: end the free roam for an event
CHALLENGE = challenge.KIND  # 19: head-to-head challenges in the world (forza_challenge.py)
STATS, STATS_ROWS = 20, 21  # Xbox LIVE stats written and read, the rows (forza_stats.py)
MAX_MATCH_RECORD = 1300
MATCH_RESULTS_MAX = 25  # sessions answered per search (Forza asks for up to 25)
SESSION_INFO_SIZE = 0x3C  # XSESSION_INFO: session id, host XNADDR, key-exchange key
MAX_PRESENCE_EXTRA = 1024

PEER_TIMEOUT = 30.0  # seconds without a HELLO before a peer is dropped
# A player dropped for silence whose game says HELLO again with its token and XUID gets its id
# back for this long (#140: a Steam Deck that slept, a game that was stopped): its XNADDR, the
# session it hosts and the others' view of it stay the same. At most GONE_MAX are kept.
GONE_KEEP = 12 * 3600.0
GONE_MAX = 5000
# A restarting server keeps its players (#93): registrations are written to
# DATA_DIR/live_state.json when they change, every STATE_REFRESH seconds and on shutdown,
# and loaded again if the file is at most STATE_MAX_AGE seconds old.
STATE_FILE = "live_state.json"
STATE_MAX_AGE = 600.0
STATE_REFRESH = 30.0
STATE_VERSION = 1
MAX_PAYLOAD = 1500
# Per-player limit on bytes the server sends for it (relayed datagrams; a broadcast counts
# once per recipient). Measured through the relay (#12): a game sends each peer one
# datagram a frame of about 100 bytes per car it runs (its own, and in an event its share
# of the CPU drivers), so a full world of 8 costs a player about 7 x fps x 108 bytes with
# the relay header: 106 KB/s at 144 fps, 181 KB/s at 240. Defaults to 256 KB/s with 1 MB
# bursts (--rate-limit).
RATE_LIMIT = 256 * 1024
RATE_BURST = 4 * RATE_LIMIT


def set_rate_limit(bytes_per_second):
    global RATE_LIMIT, RATE_BURST
    RATE_LIMIT, RATE_BURST = bytes_per_second, 4 * bytes_per_second
MAX_NAME = 15  # XUSER_NAME_SIZE minus the terminator
MAX_ROOM = 64
MAX_MOTD = 64  # keeps the WELCOME about as large as the HELLO that asks for it
# Limits against abuse from the internet (every datagram is untrusted):
MAX_PEERS_PER_IP = 16  # registered players behind one public address (a LAN, a CGNAT)
# datagrams/s and burst accepted from one address (a player in a world of 8 at 240 fps: 1680/s)
PACKETS_PER_IP = (2500, 5000)
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
        self.invite_times = []  # when its last invites went out (INVITE_LIMIT per INVITE_WINDOW)
        self.features = 0  # HELLO's features byte (FEATURE_*)
        self.friends_ok = False  # FEATURE_FRIENDS and its friends key owns the XUID's friends
        self.events_identity = None  # fresh HELLO proof; never restored from live_state.json
        self.chat_sessions = set()  # the session ids the game reports for chat (forza_chat.py)
        # Matchmaking: the session it hosts and the one it is in (see ForzaLive.on_match)
        self.match = None  # (playlist key, record, since)
        self.standby = None
        self.standby_host = False


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
        self.invites = True  # deliver game invites (--no-invites: log and drop them)
        self.dropped_packets = 0  # over a player's rate limit
        self.state_path = None  # where registrations are kept across a restart (#93)
        self.state_dirty = False
        self.state_saved = 0.0  # monotonic time of the last write
        self.restored = set()  # ids of players loaded from the state file, not heard from yet
        self.gone = {}  # token -> (id, xuid, monotonic time): players dropped for silence
        self.friends = friends.Friends(self)
        self.chat = chat.Chat(self, self.friends.store)
        self.events = events.Events(self)
        self.chat.commands = self.chat_command
        self.world_keys = {}  # world session id -> its playlist (lsp.WORLD_HOPPERS)
        lsp.client_features = self.features_of
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
            self.restored.discard(peer.id)
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
                before = (peer.match, peer.standby, peer.standby_host)
                self.on_match(peer, kind, body)
                if (peer.match, peer.standby, peer.standby_host) != before:
                    self.state_dirty = True
            elif kind == CHALLENGE and body:
                self.challenges.on_message(peer.id, body)
            elif kind == STATS and body:
                self.on_stats(peer, body)
            elif kind == FRIEND and body:
                self.friends.on_message(peer, body)
            elif kind == CHAT and body:
                self.chat.on_message(peer, body)
            elif kind == events.KIND:
                self.events.on_message(peer, body)
        except Exception as e:  # a malformed datagram must never end the server
            log.debug("bad packet from %s: %r", addr, e)

    def on_hello(self, body, addr):
        version, token, xuid, local_ip, local_port = struct.unpack_from(">HQQIH", body, 0)
        pos = 24
        room_len = body[pos]
        room = body[pos + 1:pos + 1 + room_len].decode("utf-8", "replace")[:MAX_ROOM]
        pos += 1 + room_len
        name_len = body[pos]
        name = limits.clean_gamertag(body[pos + 1:pos + 1 + name_len].decode("utf-8", "replace"), xuid)
        pos += 1 + name_len
        features = body[pos] if pos < len(body) else 0
        friend_key = b""  # after the features byte: u8 length + the friends key
        if pos + 1 < len(body):
            friend_key = body[pos + 2:pos + 2 + body[pos + 1]]
        if version != VERSION:
            self.error(addr, 2, f"server speaks protocol {VERSION}, client {version}")
            return
        if self.events.required and not features & events.FEATURE:
            self.error(addr, 5, "this server requires event-unlock support (TU4)")
            return
        if self.chat.banned(xuid):  # /admin/kick
            if take(self.error_buckets, self.limit_key(addr), ERRORS_PER_IP):
                self.error(addr, 4, "removed by the server admin")
            return
        peer = self.by_addr.get(addr)
        if peer is not None and peer.token != token:
            self.drop(peer, "restarted")  # same address, new game process
            peer = None
        if peer is None:
            peer = self.rebind_restored(addr, token, xuid)
        new = peer is None
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
            back = self.take_gone(token, xuid)
            if back is None:
                reserved = {pid for pid, _, _ in self.gone.values()}
                while self.next_id in self.peers or self.next_id == 0 or self.next_id in reserved:
                    self.next_id = (self.next_id + 1) & 0xFFFFFF
            peer = Peer(back if back is not None else self.next_id, addr, token, xuid, name, room,
                        (local_ip, local_port))
            if back is None:
                self.next_id = (self.next_id + 1) & 0xFFFFFF
            peer.proxied = proxied
            self.peers[peer.id] = peer
            self.by_addr[addr] = peer
            self.per_ip[addr[0]] = self.per_ip.get(addr[0], 0) + 1
            self.state_dirty = True
            log.info("%s  #%d %r xuid %016X room %r from %s:%d (lan %s:%d)%s",
                     "join" if back is None else "back", peer.id, name, xuid, room, addr[0],
                     addr[1], u32_to_ip(local_ip), local_port, " via proxy" if proxied else "")
        peer.seen = time.monotonic()
        self.restored.discard(peer.id)
        if ((peer.name, peer.xuid, peer.room, peer.local, peer.features) !=
                (name, xuid, room, (local_ip, local_port), features)):
            self.state_dirty = True
        peer.name, peer.xuid, peer.room, peer.local = name, xuid, room, (local_ip, local_port)
        peer.features = features
        was_ok = peer.friends_ok
        self.friends.hello(peer, friend_key)
        peer.events_identity = (peer.xuid, self.events.credential(peer)) if peer.friends_ok else None
        if peer.friends_ok != was_ok:
            self.state_dirty = True
        if new:
            self.friends.came_or_went(peer.xuid)
        # Behind a proxy the observed address is the proxy's local socket, no public address
        # of the player: report none (0.0.0.0:0), as in forward_header.
        public = (0, 0) if peer.proxied else (ip_to_u32(addr[0]), addr[1])
        body = struct.pack(">IHIHH", peer.id, self.epoch, *public, len(self.motd))
        http_port = self.proxy_http_port if peer.proxied and self.proxy_http_port else self.http_port
        welcome = body + self.motd + struct.pack(">H", http_port)
        if peer.features & events.FEATURE:
            welcome += bytes([self.events.required])
        self.send(addr, WELCOME, welcome)
        self.events.hello(peer)

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
        if (state, session, status, extra) != (peer.state, peer.session, peer.status, peer.extra):
            self.state_dirty = True
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
                if not self.invites:
                    log.info("invite #%d %r -> #%d %r dropped (invites off)", src.id, src.name,
                             peer.id, peer.name)
                    return
                if not src.features & FEATURE_INVITE_PICK or not peer.features & FEATURE_INVITE_PICK:
                    # A game up to v0.1.12 invites everyone online, and joins any invite
                    # at once.
                    log.info("invite #%d %r -> #%d %r dropped (%s game)", src.id, src.name,
                             peer.id, peer.name,
                             "inviter's" if not src.features & FEATURE_INVITE_PICK else "invitee's")
                    return
                now = time.monotonic()
                src.invite_times = [t for t in src.invite_times if now - t < INVITE_WINDOW]
                if len(src.invite_times) >= INVITE_LIMIT:
                    log.info("invite #%d %r -> #%d %r dropped (over %d invites in %d s)", src.id,
                             src.name, peer.id, peer.name, INVITE_LIMIT, INVITE_WINDOW)
                    return
                if self.friends.store.blocks(peer.xuid, src.xuid):
                    log.info("invite #%d %r -> #%d %r dropped (blocked)", src.id, src.name,
                             peer.id, peer.name)
                    return
                src.invite_times.append(now)
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
    # session id any more (or a member reports to host it: a host whose game closed stays
    # registered until it times out), its standby records stand in for it: searchers get
    # the record of the member the game made its host (the client sees the game's host
    # flag and sends op 4 instead of op 2; the latest one), else of the oldest remaining
    # member, with the number of members as the filled slots (forza_world_host.py).

    def standby_sessions(self, src, key):
        """Advertisable standby sessions for playlist key: [(peer, record, since)]."""
        room = [p for p in self.peers.values() if p.room == src.room]
        hosted = {p.match[1][:8] for p in room if getattr(p, "match", None)}
        hosted -= world_host.taken_over(room, key)
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
            members.sort(key=world_host.lead_order)
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
                world_host.note_host(peer, host)
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
        stale = world_host.taken_over([peer] + self.room_peers(peer), key)
        found = [(p, p.match[1], p.match[2]) for p in self.room_peers(peer)
                 if getattr(p, "match", None) and p.match[0] == key
                 and not (own and own[0] == key and p.match[2] >= own[2])
                 and p.match[1][:8] not in stale]
        found += self.standby_sessions(peer, key)
        found = [f for f in found if self.match_slots(f[1])[1] > 0]
        if key == lsp.COOP_HOPPER:
            found = self.coop_placement(peer, found)
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
                        allowed=set(room), verified=bool(getattr(peer, "friends_ok", False)))
        elif body[0] == 2:
            names = {p.xuid: p.name for p in self.peers.values()}
            reply = stats.read(peer.xuid, body[1:], lambda x: names.get(x) or lsp.gamertag_of(x))
            if self.spend(peer, len(reply)):
                self.send(peer.addr, STATS_ROWS, reply)

    # --- Horizon World events (forza_world.py) ---

    def world_sessions(self):
        """The Horizon World and Co-op Festival sessions: {session id: (its host peer,
        humans)} (self.world_keys says which playlist). A session whose host left is led by
        its standby host (see standby_sessions)."""
        worlds = {}
        keys = {}
        for key in lsp.WORLD_HOPPERS:
            found = {}
            stale = world_host.taken_over(self.peers.values(), key)
            for p in self.peers.values():
                match = getattr(p, "match", None)
                if match and match[0] == key and match[1][:8] not in stale:
                    found[match[1][:8]] = (p, self.match_slots(match[1])[0])
            members = {}
            for p in self.peers.values():
                standby = getattr(p, "standby", None)
                if standby and standby[0] == key and standby[1][:8] not in found:
                    members.setdefault(standby[1][:8], []).append(p)
            for sid, group in members.items():
                group.sort(key=world_host.lead_order)
                found[sid] = (group[0], len(group))
            for sid in found:
                keys[sid] = key
            worlds.update(found)
        self.world_keys = keys
        return worlds

    def world_members(self, sid):
        """The players in a world (hosting it or in it): [peer]."""
        return [p for p in self.peers.values() if self.world_of_peer(p.id) == sid]

    def world_of(self, xuid):
        """(session id, humans) of the world the player with this XUID hosts, or None."""
        for sid, (host, humans) in self.world_sessions().items():
            if host.xuid == xuid:
                return sid, humans
        return None

    def world_tick(self):
        """Every 2 s: tell each world's host about an event waiting for it (op u8 1,
        event id u32); the client ends the free roam once the event is its next round. A
        campaign round's CPU skill goes to every console of its world (op 2,
        forza_campaign.ai_message) until the round starts."""
        worlds = self.world_sessions()
        scheduled = {sid for sid in worlds if self.world_keys.get(sid) == lsp.WORLD_HOPPER}
        for sid, event in world.WORLDS.tick(set(worlds), scheduled):
            host = worlds[sid][0]
            self.send(host.addr, WORLD_EVENT, struct.pack(">BI", 1, event[0]) + sid)
        for sid, event, code in world.WORLDS.empty:  # a campaign round nobody raced
            cars = (": its cars must be %s" % event.restriction_text()) if event is not None else ""
            for p in self.world_members(sid):
                if p.features & chat.FEATURE:
                    self.chat.notice(p, "%s ended at once and nobody raced it%s (a game that has no "
                                        "such car shows NO CARS AVAILABLE)" % (code, cars))
        world.WORLDS.empty = []
        for sid, st in world.WORLDS.dropped:  # a campaign the host's game never took
            for p in self.world_members(sid):
                if p.features & chat.FEATURE:
                    self.chat.notice(p, "%s did not start: the world's host must be driving in "
                                        "the free roam. Try /event again" % st["campaign"].code)
        coop_ledger.LEDGER.maybe_save()
        world.WORLDS.members = {sid: {p.xuid: p.name for p in self.world_members(sid)}
                                for sid in worlds}
        for sid, event in world.WORLDS.campaigns():
            body = campaign.ai_message(event, sid)
            for p in self.world_members(sid):
                self.send(p.addr, WORLD_EVENT, body)

    def world_request(self, session, event_id):
        """GET /world/event?session=<id hex>|all[&event=<id>]: start an event in a world.
        Co-op Festival worlds are left out: their events are their host's (/event)."""
        worlds = self.world_sessions()
        worlds = {sid: w for sid, w in worlds.items() if self.world_keys.get(sid) != lsp.COOP_HOPPER}
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
            if record and record[0] in lsp.WORLD_HOPPERS:
                return bytes(record[1][:8])
        return None

    # --- the Co-op Festival (forza_campaign.py, #309) ---

    def features_of(self, xuid):
        """The HELLO features of the game signed in with this XUID (lsp.client_features),
        or None when it is not online."""
        found = [p.features for p in self.peers.values() if p.xuid == xuid]
        return max(found) if found else None

    def coop_placement(self, peer, found):
        """Co-op Festival placement: only worlds led by one of the searcher's friends (the
        friends list kept by forza_friends.py, accepted both ways), both of them proven by
        their friends keys (friends_ok: a HELLO's XUID alone could be anyone's). Without one
        the searcher's game opens a world of its own, which its friends are then placed
        into."""
        store = self.friends.store
        if not getattr(peer, "friends_ok", False):
            log.info("match #%d %r co-op: its XUID is not proven (friends key): no world", peer.id,
                     peer.name)
            return []
        kept = [f for f in found if getattr(f[0], "friends_ok", False)
                and store.relation(peer.xuid, f[0].xuid) == friends.FRIEND]
        if len(kept) != len(found):
            log.info("match #%d %r co-op: %d of %d world(s) led by a friend", peer.id,
                     peer.name, len(kept), len(found))
        return kept

    def chat_command(self, peer, channel, text):
        """A chat line starting with "/" (forza_chat.Chat.say): True when it was a command
        (answered with notices, not relayed). /event and /events, in WORLD chat."""
        word = text.split()[0].lower()
        if word not in ("/event", "/events"):
            return False
        arg = text[len(word):].strip()
        if channel != chat.WORLD:
            self.chat.notice(peer, "Type /event in World chat, in a Co-op Festival world")
            return True
        if word == "/events" or not arg or arg.lower() == "help":
            if word == "/event":
                self.chat.notice(peer, campaign.USAGE)
            level = arg.lower() if arg.lower() == "street" else int(arg) if arg.isdigit() else None
            if word == "/events":
                for line in campaign.listing(level):
                    self.chat.notice(peer, line)
            return True
        sid = self.world_of_peer(peer.id)
        worlds = self.world_sessions()
        if sid is None or sid not in worlds:
            self.chat.notice(peer, "You are not in a Horizon World")
        elif self.world_keys.get(sid) != lsp.COOP_HOPPER:
            self.chat.notice(peer, "Events start from a Co-op Festival world (Find Game)")
        elif worlds[sid][0].xuid != peer.xuid:
            self.chat.notice(peer, "Only the world's host (%s) can start an event" %
                             worlds[sid][0].name)
        elif world.WORLDS.state.get(sid):
            self.chat.notice(peer, "An event is already starting in this world")
        elif coop_ledger.LEDGER.open_round(sid):
            self.chat.notice(peer, "An event is running in this world: start the next one when "
                                   "you are back in the free roam")
        elif coop_ledger.LEDGER.waiting_for(peer.xuid) >= coop_ledger.MAX_WAITING:
            self.chat.notice(peer, coop_ledger.LEDGER.refusal(peer.xuid))
        else:
            event = campaign.find(arg)
            if event is None:
                self.chat.notice(peer, "No festival or street race %r to race online (/events "
                                       "lists them)" % arg[:20])
                return True
            world.WORLDS.request(sid, campaign=event, by=peer.name, by_xuid=peer.xuid)
            text = "%s starts %s" % (peer.name, event.describe())
            if event.loaners == 0:  # the lobby lends a stock car that meets it: there is none
                text += (". No stock car fits it (%s): only players whose own upgraded car "
                         "fits can race it" % event.restriction_text())
            for p in self.world_members(sid):
                if p.features & chat.FEATURE:
                    self.chat.notice(p, text)
        return True

    # --- keeping the players across a server restart (#93) ---
    # A restart used to forget everyone: the games' next packet got "not registered", they
    # signed in again under NEW player ids and a lobby's members lost each other (each
    # game keeps the other's old id), and the two halves of a Horizon World each became a
    # host. The server now writes who is registered (id, token, XUID, gamertag, room,
    # addresses, presence, hosted and joined sessions, the id counter and the epoch) to the
    # data folder, and a server that starts within STATE_MAX_AGE seconds loads it. The games
    # change nothing: they keep sending HELLO with the same token every 5 s from the same
    # socket, which finds the restored player and gets the same id and epoch back.

    def snapshot(self):
        now_m, now_w = time.monotonic(), time.time()

        def wall(mono):  # monotonic timestamps do not survive a restart
            return now_w - (now_m - mono)

        def sess(s):
            return None if not s else [s[0], s[1].hex(), wall(s[2])]

        peers = []
        for p in self.peers.values():
            peers.append({
                "id": p.id, "addr": list(p.addr), "token": p.token, "xuid": p.xuid,
                "name": p.name, "room": p.room, "local": list(p.local), "proxied": p.proxied,
                "since": p.since, "state": p.state, "session": p.session.hex(),
                "status": p.status, "extra": p.extra.hex(), "features": p.features,
                "friends_ok": p.friends_ok,
                "match": sess(p.match), "standby": sess(p.standby),
                "standby_host": p.standby_host})
        return {"version": STATE_VERSION, "saved_at": now_w, "epoch": self.epoch,
                "next_id": self.next_id, "peers": peers}

    def save_state(self):
        """Writes the registrations to the state file (atomically). False on failure."""
        if not self.state_path:
            return False
        try:
            data = json.dumps(self.snapshot())
            tmp = self.state_path + ".tmp"
            with open(tmp, "w") as f:
                f.write(data)
            os.replace(tmp, self.state_path)
        except OSError as e:
            log.warning("cannot write %s: %s", self.state_path, e)
            return False
        self.state_dirty = False
        self.state_saved = time.monotonic()
        return True

    def maybe_save_state(self):
        """Every second from the main loop: write after a change (at most every 2 s), and
        every STATE_REFRESH s while players are on, which keeps the file young."""
        if not self.state_path:
            return
        age = time.monotonic() - self.state_saved
        if (self.state_dirty and age >= 2) or (self.peers and age >= STATE_REFRESH):
            self.save_state()

    def load_state(self, max_age=STATE_MAX_AGE):
        """Restores the players of the state file if it is young enough. Returns how many."""
        if not self.state_path or self.peers:
            return 0
        try:
            with open(self.state_path) as f:
                state = json.load(f)
            age = time.time() - state["saved_at"]
            if state["version"] != STATE_VERSION or not -60 <= age <= max_age:
                log.info("state file ignored (%s)", "version" if state["version"] != STATE_VERSION
                         else "%d s old" % age)
                return 0
            now_m, now_w = time.monotonic(), time.time()

            def sess(s):
                return None if not s else (s[0], bytes.fromhex(s[1]), now_m - (now_w - s[2]))

            for d in state["peers"]:
                addr = (d["addr"][0], d["addr"][1])
                if d["id"] in self.peers or addr in self.by_addr:
                    continue
                peer = Peer(d["id"], addr, d["token"], d["xuid"],
                            limits.clean_gamertag(d["name"], d["xuid"]), d["room"],
                            tuple(d["local"]))
                peer.proxied = d["proxied"]
                peer.since = d["since"]
                peer.state, peer.session = d["state"], bytes.fromhex(d["session"])
                peer.status, peer.extra = d["status"], bytes.fromhex(d["extra"])
                peer.features = d["features"]
                peer.friends_ok = d.get("friends_ok", False)
                peer.match, peer.standby = sess(d["match"]), sess(d["standby"])
                peer.standby_host = d["standby_host"]
                self.peers[peer.id] = peer
                self.by_addr[addr] = peer
                self.per_ip[addr[0]] = self.per_ip.get(addr[0], 0) + 1
                self.restored.add(peer.id)
            self.epoch = state["epoch"]
            self.next_id = max([state["next_id"]] + [p + 1 for p in self.peers]) & 0xFFFFFF or 1
        except (OSError, ValueError, KeyError, TypeError, IndexError) as e:
            if not isinstance(e, FileNotFoundError):
                log.warning("cannot read %s: %s", self.state_path, e)
            return 0
        log.info("restored %d player(s) from %s (%d s old, epoch %04X)", len(self.peers),
                 self.state_path, age, self.epoch)
        return len(self.peers)

    def rebind_restored(self, addr, token, xuid):
        """A HELLO from a new address whose token and XUID match a player restored from the
        state file that has not been heard from since: its NAT mapping changed during the
        restart. It keeps its id. (Only restored players: a live registration is never
        taken over from another address.)"""
        for pid in list(self.restored):
            peer = self.peers.get(pid)
            if peer is None or peer.token != token or peer.xuid != xuid:
                continue
            if self.by_addr.get(peer.addr) is peer:
                del self.by_addr[peer.addr]
                left = self.per_ip.get(peer.addr[0], 1) - 1
                if left > 0:
                    self.per_ip[peer.addr[0]] = left
                else:
                    self.per_ip.pop(peer.addr[0], None)
            log.info("rebind #%d %r %s:%d -> %s:%d", peer.id, peer.name, *peer.addr, *addr)
            peer.addr = addr
            self.by_addr[addr] = peer
            self.per_ip[addr[0]] = self.per_ip.get(addr[0], 0) + 1
            self.restored.discard(pid)
            self.state_dirty = True
            return peer
        return None

    def take_gone(self, token, xuid):
        """The id of a player dropped for silence that says HELLO again with its token and XUID
        (GONE_KEEP), or None."""
        entry = self.gone.pop(token, None)
        if entry is None:
            return None
        pid, gone_xuid, when = entry
        if gone_xuid != xuid or time.monotonic() - when > GONE_KEEP or pid in self.peers:
            return None
        return pid

    def drop(self, peer, why):
        if why == "timed out":
            self.gone[peer.token] = (peer.id, peer.xuid, time.monotonic())
            while len(self.gone) > GONE_MAX:
                del self.gone[next(iter(self.gone))]  # the oldest
        self.challenges.drop(peer.id)
        self.peers.pop(peer.id, None)
        self.restored.discard(peer.id)
        self.state_dirty = True
        if self.by_addr.get(peer.addr) is peer:
            del self.by_addr[peer.addr]
            left = self.per_ip.get(peer.addr[0], 1) - 1
            if left > 0:
                self.per_ip[peer.addr[0]] = left
            else:
                self.per_ip.pop(peer.addr[0], None)
        log.info("leave #%d %r (%s)", peer.id, peer.name, why)
        self.friends.came_or_went(peer.xuid)

    def expire(self):
        now = time.monotonic()
        for peer in [p for p in self.peers.values() if now - p.seen > PEER_TIMEOUT]:
            self.drop(peer, "timed out")
        for token in [t for t, (_, _, when) in self.gone.items() if now - when > GONE_KEEP]:
            del self.gone[token]
        for buckets in (self.packet_buckets, self.hello_buckets, self.error_buckets):
            if len(buckets) > 5000:  # forgotten addresses (forged sources)
                for ip in [ip for ip, (_, last) in buckets.items() if now - last > 60]:
                    del buckets[ip]

    @staticmethod
    def match_game_type(record):
        """The game type a custom game's record carries (its property 0x10000030, which Create
        Custom Game sets and Find Game does not), or None."""
        if len(record) < SESSION_INFO_SIZE + 5:
            return None
        pos = SESSION_INFO_SIZE + 4
        count, pos = record[pos], pos + 1
        for _ in range(count):
            if pos + 13 > len(record):
                return None
            pid, = struct.unpack_from(">I", record, pos)
            if pid == 0x10000030:
                return struct.unpack_from(">I", record, pos + 5)[0]
            pos += 13
        return None

    def match_status(self, match):
        """A published session for status(): playlist, session id, slots and game type."""
        if not match:
            return None
        filled, free = self.match_slots(match[1])
        return {"playlist": match[0], "session": match[1][:8].hex(), "players": filled,
                "slots": filled + free, "game_type": self.match_game_type(match[1])}

    def status(self):
        rooms = {}
        for p in self.peers.values():
            rooms.setdefault(p.room, []).append(
                {"id": p.id, "gamertag": p.name, "xuid": f"{p.xuid:016X}",
                 "online_for": int(time.time() - p.since), "relayed_bytes": p.relayed_bytes,
                 "via_proxy": p.proxied,
                 "state": f"{p.state:08X}", "session": p.session[:8].hex(), "status": p.status,
                 "match": self.match_status(getattr(p, "match", None)),
                 "standby": self.match_status(getattr(p, "standby", None))})
        worlds = {sid.hex(): {"host": host.name, "humans": humans,
                              "event": world.WORLDS.state.get(sid)}
                  for sid, (host, humans) in self.world_sessions().items()}
        return {"server": "forza-live", "protocol": VERSION, "uptime": int(time.time() - self.started),
                "players": len(self.peers),
                "via_proxy": sum(1 for p in self.peers.values() if p.proxied),
                "relayed_packets": self.relayed_packets,
                "dropped_packets": self.dropped_packets, "rooms": rooms, "worlds": worlds,
                **self.challenges.status(), **self.friends.status(),
                "chat": {"with_chat": len(self.chat.chatters()), "kept": len(self.chat.ring)}}


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
            elif path.startswith("/admin/"):
                # Chat moderation (forza_chat.py): /admin/chat, /admin/mute, /admin/kick.
                url = urllib.parse.urlsplit(path)
                query = urllib.parse.parse_qs(url.query)
                if not lsp.is_admin(peer, headers, query):
                    reply = response(403, b"forbidden", "text/plain", conn)
                elif url.path == "/admin/events" or url.path.startswith("/admin/events/"):
                    try:
                        if "origin" in headers:
                            raise ValueError("use the organizer tool rather than a cross-origin browser request")
                        if method == "POST" and headers.get("content-type", "").split(";")[0] != "application/json":
                            raise ValueError("event changes require application/json")
                        if len(body) > 4096:
                            raise ValueError("event request is too large")
                        data = json.loads(body) if body else {}
                        if not isinstance(data, dict):
                            raise ValueError("expected a JSON object")
                        result = server.events.handle(method, url.path, data)
                        reply = response(200, json.dumps(result).encode(), "application/json", conn)
                    except (ValueError, TypeError, sqlite3.IntegrityError) as error:
                        reply = response(400, json.dumps({"error": str(error)}).encode(), "application/json", conn)
                    except (OSError, sqlite3.Error):
                        log.exception("community event storage unavailable")
                        reply = response(500, b"event storage unavailable", "text/plain", conn)
                elif url.path == "/admin/chat":
                    reply = response(200, json.dumps(server.chat.moderation(), indent=2).encode(),
                                     "application/json", conn)
                elif url.path in ("/admin/mute", "/admin/kick"):
                    try:
                        minutes = float(query.get("minutes", ["10"])[0])
                    except ValueError:
                        minutes = -1
                    result = None if minutes < 0 else server.chat.admin(
                        url.path[7:], query.get("who", [""])[0], minutes)
                    if minutes < 0:
                        reply = response(400, b"minutes: a number, 0 lifts it", "text/plain", conn)
                    elif result is None:
                        reply = response(404, b"no such player", "text/plain", conn)
                    else:
                        reply = response(200, json.dumps(result, indent=2).encode(),
                                         "application/json", conn)
                else:
                    reply = response(404, b"not found", "text/plain", conn)
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
    parser.add_argument("--event-unlocks", action="store_true",
                        help="require TU4 event-unlock clients; enrolled event rewards permit unicorns online")
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
    parser.add_argument("--no-invites", action="store_true",
                        default=os.environ.get("FORZA_INVITES", "1") == "0",
                        help="log and drop every game invite (#90; env FORZA_INVITES=0). "
                             "Without it invites are delivered only between games that ask "
                             "before joining (FEATURE_INVITE_PICK), at most 4 per inviter in 30 s")
    parser.add_argument("-v", "--verbose", action="store_true")
    lsp.add_arguments(parser)
    world.add_arguments(parser)
    campaign.add_arguments(parser)
    challenge.add_arguments(parser)
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s")
    limits.configure_proxies(os.environ.get("FORZA_TRUSTED_PROXIES", ""), *args.trusted_proxy)
    if limits.TRUSTED_PROXIES:
        log.info("trusted proxies: %s", ", ".join(map(str, limits.TRUSTED_PROXIES)))
    lsp.configure(args)
    world.configure(args)
    campaign.configure(args, args.game_db)
    coop_ledger.configure(args.data_dir)
    stats.on_skill_write = coop_ledger.LEDGER.host_score
    challenge.configure(args)
    set_rate_limit(args.rate_limit)

    loop = asyncio.get_running_loop()
    transport, server = await loop.create_datagram_endpoint(
        lambda: ForzaLive(args.motd, args.max_peers), local_addr=(args.host, args.port))
    server.state_path = os.path.join(args.data_dir, STATE_FILE)
    server.load_state()
    server.friends.store.path = os.path.join(args.data_dir, friends.FILE)
    server.friends.store.load()
    server.events.db.close()
    server.events = events.Events(server, os.path.join(args.data_dir, "events.sqlite3"), args.event_unlocks)
    server.chat.path = os.path.join(args.data_dir, chat.FILE)
    server.chat.load()
    log.info("Forza Live listening on UDP %s:%d (epoch %04X)", args.host, args.port, server.epoch)
    if args.http_port:
        server.http_port = args.http_port
        server.proxy_http_port = args.proxy_http_port
        server.invites = not args.no_invites
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
            server.maybe_save_state()
            server.friends.store.maybe_save()
            try:
                server.events.flush()
            except (OSError, sqlite3.Error):
                log.warning("community event delivery storage unavailable; retrying later")
            if ticks % 30 == 0 and discord_status.enabled():
                # urllib blocks: off the event loop, and never fatal (it catches everything)
                await asyncio.to_thread(discord_status.update, server.status(), args.data_dir)
    finally:
        server.save_state()  # a restart now keeps the players (#93)
        if coop_ledger.LEDGER.dirty:
            coop_ledger.LEDGER.save()
        if server.friends.store.dirty:
            server.friends.store.save()
        if discord_status.enabled():
            discord_status.offline(args.data_dir)
        transport.close()
        server.events.db.close()


if __name__ == "__main__":
    # SIGTERM (docker stop) ends the server like Ctrl+C; as a container's first process
    # it would otherwise be ignored.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
