"""Friends on the Forza Live server (#92): requests by gamertag, accept or decline, remove
and block, kept per player (XUID) in DATA_DIR/friends.json.

Games up to v0.1.14 count every player in their room as a friend (the room's roster,
PEERS). A game that sends FEATURE_FRIENDS in its HELLO asks the server for its friends
list instead and shows only those players in the game's friends list (Xbox Guide friends,
presence, join a friend) and first in the invite picker. Everything else stays as it was:
the roster still lists the whole room (matchmaking, QoS and stats need it), and old games
neither send nor get FRIEND messages.

Who is who: the XUID a game reports is not proven, so the first game that uses the friends
of an XUID binds them to its friends key (16 random bytes in its live_identity.txt, sent
after the HELLO features byte; the server keeps a SHA-256 of it). A game with the same XUID
and another key gets no friends list and cannot change it.

Message FRIEND (UDP kind 22), body op u8 then (big endian; strings u8 length + UTF-8):
  client -> server
    1 LIST     version u32 (of the last list it got, 0 for none)
    2 SEARCH   request u16, query
    3 REQUEST  xuid u64 (0: find the gamertag), gamertag
    4 ACCEPT   xuid u64
    5 DECLINE  xuid u64 (an incoming request; also takes back one we sent)
    6 REMOVE   xuid u64
    7 BLOCK    xuid u64, gamertag
    8 UNBLOCK  xuid u64
  server -> client
    16 LIST    version u32, total u16, first u16, count u16, then count entries:
               xuid u64, relation u8 (1 friend, 2 asks us, 3 we asked, 4 blocked),
               state u32 (a friend's X_ONLINE_FRIENDSTATE flags, 0 offline), gamertag,
               status (a friend's presence line). In parts that fit a datagram.
    17 SAME    version u32: the list has not changed
    18 RESULTS request u16, count u8, then xuid u64, online u8, relation u8 (0 none),
               gamertag
    19 NOTICE  event u8 (1 a request from, 2 accepted by, 3 text), xuid u64, gamertag, text

Limits: MAX_FRIENDS friends, MAX_PENDING requests waiting each way, MAX_BLOCKED blocks;
REQUESTS_PER_MIN requests sent, ACTIONS_PER_MIN other changes, SEARCHES_PER_MIN searches
per player. A request to a player who blocked you looks sent but is dropped.
"""

import hashlib
import json
import logging
import os
import struct
import time
import zlib

log = logging.getLogger("forza-live")

KIND = 22
FEATURE = 0x02  # HELLO features byte: the game uses the server's friends list
LIST, SEARCH, REQUEST, ACCEPT, DECLINE, REMOVE, BLOCK, UNBLOCK = range(1, 9)
R_LIST, R_SAME, R_RESULTS, R_NOTICE = 16, 17, 18, 19
FRIEND, INCOMING, OUTGOING, BLOCKED = 1, 2, 3, 4
N_REQUEST, N_ACCEPTED, N_TEXT = 1, 2, 3

FILE = "friends.json"
MAX_FRIENDS = 200
MAX_PENDING = 50
MAX_BLOCKED = 200
MAX_RESULTS = 20
REQUESTS_PER_MIN = 10
ACTIONS_PER_MIN = 60
SEARCHES_PER_MIN = 30
LISTS = (0.5, 4)  # list answers per second and burst
FORGET_DAYS = 180  # a player with no friends, requests or blocks is forgotten after this
KEY_SIZE = 16
STATUS_MAX = 60


def text8(s):
    b = s.encode()[:255]
    while True:
        try:
            b.decode()
            break
        except UnicodeDecodeError:
            b = b[:-1]
    return bytes([len(b)]) + b


def read8(body, pos):
    n = body[pos]
    return body[pos + 1:pos + 1 + n].decode("utf-8", "replace"), pos + 1 + n


def key_hash(key):
    return hashlib.sha256(bytes(key)).hexdigest()


class Store:
    """The players' relations: {xuid: {"name", "seen", "key", "friends", "in", "out",
    "blocked"}}, each relation a {xuid hex: since} map, kept symmetric (A's "out" is B's
    "in")."""

    def __init__(self, path=None):
        self.path = path
        self.players = {}
        self.dirty = False
        self.saved = 0.0

    @staticmethod
    def hx(xuid):
        return "%016X" % xuid

    def get(self, xuid, create=False):
        k = self.hx(xuid)
        p = self.players.get(k)
        if p is None and create:
            p = self.players[k] = {"name": "", "seen": time.time(), "key": "", "friends": {},
                                   "in": {}, "out": {}, "blocked": {}}
            self.dirty = True
        return p

    def relation(self, a, b):
        """What b is to a (FRIEND, INCOMING, OUTGOING, BLOCKED, or 0)."""
        p = self.get(a)
        if not p:
            return 0
        k = self.hx(b)
        for rel, field in ((BLOCKED, "blocked"), (FRIEND, "friends"), (INCOMING, "in"),
                           (OUTGOING, "out")):
            if k in p[field]:
                return rel
        return 0

    def blocks(self, a, b):
        """a has blocked b."""
        p = self.get(a)
        return bool(p) and self.hx(b) in p["blocked"]

    def seen(self, xuid, name):
        p = self.get(xuid, create=True)
        if name and p["name"] != name:
            p["name"] = name
            self.dirty = True
        if time.time() - p["seen"] > 3600:
            p["seen"] = time.time()
            self.dirty = True

    def claim(self, xuid, key):
        """True if this friends key may use the XUID's friends (binding it on first use)."""
        if len(key) != KEY_SIZE:
            return False
        p = self.get(xuid, create=True)
        h = key_hash(key)
        if not p["key"]:
            p["key"] = h
            self.dirty = True
            return True
        return p["key"] == h

    def unlink(self, a, b):
        """Drops every relation between a and b except blocks."""
        pa, pb = self.get(a), self.get(b)
        ka, kb = self.hx(a), self.hx(b)
        changed = False
        for p, k in ((pa, kb), (pb, ka)):
            if not p:
                continue
            for field in ("friends", "in", "out"):
                if p[field].pop(k, None) is not None:
                    changed = True
        self.dirty |= changed
        return changed

    def befriend(self, a, b):
        self.unlink(a, b)
        now = time.time()
        self.get(a, True)["friends"][self.hx(b)] = now
        self.get(b, True)["friends"][self.hx(a)] = now
        self.dirty = True

    def ask(self, a, b):
        now = time.time()
        self.get(a, True)["out"][self.hx(b)] = now
        self.get(b, True)["in"][self.hx(a)] = now
        self.dirty = True

    def block(self, a, b):
        self.unlink(a, b)
        self.get(a, True)["blocked"][self.hx(b)] = time.time()
        self.dirty = True

    def unblock(self, a, b):
        p = self.get(a)
        if p and p["blocked"].pop(self.hx(b), None) is not None:
            self.dirty = True
            return True
        return False

    def find(self, name):
        """XUIDs of the players with this gamertag (any case), most recently seen first."""
        name = name.lower()
        found = [(p["seen"], int(k, 16)) for k, p in self.players.items()
                 if p["name"] and p["name"].lower() == name]
        return [x for _, x in sorted(found, reverse=True)]

    def name(self, xuid):
        p = self.get(xuid)
        return p["name"] if p and p["name"] else "%016X" % xuid

    # --- the file ---

    def load(self):
        if not self.path:
            return
        try:
            with open(self.path) as f:
                data = json.load(f)
            players = data["players"]
            cutoff = time.time() - FORGET_DAYS * 86400
            self.players = {k: p for k, p in players.items()
                            if p["friends"] or p["in"] or p["out"] or p["blocked"]
                            or p["seen"] >= cutoff}
            log.info("friends: %d player(s) from %s", len(self.players), self.path)
        except FileNotFoundError:
            pass
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as e:
            log.warning("cannot read %s: %s", self.path, e)

    def save(self):
        if not self.path:
            return False
        try:
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"version": 1, "players": self.players}, f)
            os.replace(tmp, self.path)
        except OSError as e:
            log.warning("cannot write %s: %s", self.path, e)
            return False
        self.dirty = False
        self.saved = time.monotonic()
        return True

    def maybe_save(self):
        if self.dirty and time.monotonic() - self.saved >= 2:
            self.save()


class Friends:
    """The FRIEND messages. `server` is the ForzaLive (its peers, send and spend)."""

    def __init__(self, server, path=None):
        self.server = server
        self.store = Store(path)
        self.buckets = {}  # (xuid, what) -> token bucket

    # --- who is online ---

    def online(self, xuid):
        """The peers signed in with this XUID."""
        return [p for p in self.server.peers.values() if p.xuid == xuid]

    def users(self, xuid):
        """The XUID's peers that use the friends list (and own it)."""
        return [p for p in self.online(xuid)
                if p.features & FEATURE and getattr(p, "friends_ok", False)]

    def limited(self, xuid, what, per_minute, burst=None):
        rate = per_minute / 60.0
        burst = burst or max(1, per_minute // 2)
        now = time.monotonic()
        tokens, last = self.buckets.get((xuid, what), (burst, now))
        tokens = min(burst, tokens + (now - last) * rate)
        ok = tokens >= 1
        self.buckets[(xuid, what)] = (tokens - 1 if ok else tokens, now)
        if len(self.buckets) > 20000:
            self.buckets = {k: v for k, v in self.buckets.items() if now - v[1] < 120}
        return not ok

    # --- HELLO ---

    def hello(self, peer, key):
        """A HELLO with FEATURE: records the gamertag and checks the friends key."""
        if not peer.features & FEATURE:
            peer.friends_ok = False
            return
        self.store.seen(peer.xuid, peer.name)
        ok = self.store.claim(peer.xuid, key)
        if ok != getattr(peer, "friends_ok", None):
            if not ok:
                log.info("friends #%d %r: XUID %016X has another friends key", peer.id,
                         peer.name, peer.xuid)
            peer.friends_ok = ok

    def came_or_went(self, xuid):
        """A player signed in or out: its friends' lists show it."""
        p = self.store.get(xuid)
        if not p:
            return
        for k in list(p["friends"])[:MAX_FRIENDS]:
            self.push(int(k, 16))

    # --- the list ---

    def entries(self, xuid):
        p = self.store.get(xuid)
        if not p:
            return []
        rows = []
        for rel, field in ((FRIEND, "friends"), (INCOMING, "in"), (OUTGOING, "out"),
                           (BLOCKED, "blocked")):
            for k in p[field]:
                x = int(k, 16)
                state, status = 0, ""
                if rel == FRIEND:
                    on = self.online(x)
                    if on:
                        state, status = on[0].state or 1, on[0].status
                rows.append((rel, 0 if state else 1, self.store.name(x).lower(), x, state,
                             status))
        rows.sort()
        return [struct.pack(">QBI", x, rel, state) + text8(self.store.name(x)) +
                text8(status[:STATUS_MAX]) for rel, _, _, x, state, status in rows]

    def send_list(self, peer, known=0):
        entries = self.entries(peer.xuid)
        version = zlib.crc32(b"".join(entries)) or 1
        if known == version:
            self.server.send(peer.addr, KIND, struct.pack(">BI", R_SAME, version))
            return
        start = 0
        while True:
            part, size = [], 11
            while start + len(part) < len(entries) and size + len(entries[start + len(part)]) <= 1200:
                size += len(entries[start + len(part)])
                part.append(entries[start + len(part)])
            self.server.send(peer.addr, KIND, struct.pack(">BIHHH", R_LIST, version, len(entries),
                                                          start, len(part)) + b"".join(part))
            start += len(part)
            if start >= len(entries) or not part:
                break

    def push(self, xuid):
        for peer in self.users(xuid):
            self.send_list(peer)

    def notice(self, xuid, event, other, text=""):
        body = struct.pack(">BBQ", R_NOTICE, event, other) + text8(
            self.store.name(other) if other else "") + text8(text)
        for peer in self.users(xuid):
            self.server.send(peer.addr, KIND, body)

    def tell(self, peer, text):
        self.server.send(peer.addr, KIND, struct.pack(">BBQ", R_NOTICE, N_TEXT, 0) + text8("") +
                         text8(text))

    # --- messages ---

    def on_message(self, peer, body):
        if not peer.features & FEATURE:
            return
        if not getattr(peer, "friends_ok", False):
            if not self.limited(peer.xuid, "denied", 2):
                self.tell(peer, "Friends are off: this XUID's friends belong to another "
                                "installation (live_identity.txt)")
            return
        op, me = body[0], peer.xuid
        if op == LIST:
            if len(body) >= 5 and not self.limited(me, "list", LISTS[0] * 60, LISTS[1]):
                self.send_list(peer, struct.unpack_from(">I", body, 1)[0])
        elif op == SEARCH:
            if not self.limited(me, "search", SEARCHES_PER_MIN, 10):
                (request,) = struct.unpack_from(">H", body, 1)
                query, _ = read8(body, 3)
                self.search(peer, request, query)
        elif op in (REQUEST, ACCEPT, DECLINE, REMOVE, BLOCK, UNBLOCK):
            (other,) = struct.unpack_from(">Q", body, 1)
            name = read8(body, 9)[0] if op in (REQUEST, BLOCK) and len(body) > 9 else ""
            if op == REQUEST:
                if self.limited(me, "request", REQUESTS_PER_MIN):
                    self.tell(peer, "Too many friend requests; wait a minute")
                    return
            elif self.limited(me, "action", ACTIONS_PER_MIN, 30):
                self.tell(peer, "Too many changes; wait a minute")
                return
            if op == REQUEST:
                self.request(peer, other, name)
            elif op == ACCEPT:
                self.accept(peer, other)
            elif op == DECLINE:
                self.decline(peer, other)
            elif op == REMOVE:
                self.remove(peer, other)
            elif op == BLOCK:
                self.block(peer, other, name)
            else:
                self.unblock(peer, other)

    def resolve(self, peer, xuid, name):
        """The XUID a request names: given, else the player with that gamertag (online in
        the same room first, then online, then the most recently seen)."""
        if xuid:
            if self.store.get(xuid) or self.online(xuid):
                return xuid
            return 0
        name = name.strip()
        if not name:
            return 0
        on = sorted((p for p in self.server.peers.values()
                     if p.name.lower() == name.lower() and p.xuid != peer.xuid),
                    key=lambda p: p.room != peer.room)
        if on:
            return on[0].xuid
        found = [x for x in self.store.find(name) if x != peer.xuid]
        return found[0] if found else 0

    def request(self, peer, other, name):
        me = peer.xuid
        other = self.resolve(peer, other, name)
        if not other:
            self.tell(peer, "No player named %s" % (name or "that"))
            return
        if other == me:
            self.tell(peer, "That's you")
            return
        for p in self.online(other):  # it may have signed in before it had the feature
            self.store.seen(other, p.name)
        tag = self.store.name(other)
        rel = self.store.relation(me, other)
        if rel == FRIEND:
            self.tell(peer, "%s is already your friend" % tag)
            return
        if rel == BLOCKED:
            self.tell(peer, "You blocked %s; unblock them first" % tag)
            return
        if rel == INCOMING:  # they asked us: that's a yes
            self.accept(peer, other)
            return
        if rel == OUTGOING:
            self.tell(peer, "You already asked %s" % tag)
            return
        mine, theirs = self.store.get(me, True), self.store.get(other, True)
        if len(mine["friends"]) >= MAX_FRIENDS:
            self.tell(peer, "Your friends list is full (%d)" % MAX_FRIENDS)
            return
        if len(mine["out"]) >= MAX_PENDING:
            self.tell(peer, "You have %d requests waiting; cancel some first" % MAX_PENDING)
            return
        if self.store.blocks(other, me):
            log.info("friends #%d %r -> %r: request dropped (blocked)", peer.id, peer.name, tag)
            self.tell(peer, "Friend request sent to %s" % tag)
            return
        if len(theirs["in"]) >= MAX_PENDING or len(theirs["friends"]) >= MAX_FRIENDS:
            self.tell(peer, "%s can't take more friend requests right now" % tag)
            return
        self.store.ask(me, other)
        log.info("friends #%d %r asks %r (%016X)", peer.id, peer.name, tag, other)
        self.tell(peer, "Friend request sent to %s" % tag)
        self.notice(other, N_REQUEST, me)
        self.push(me)
        self.push(other)

    def accept(self, peer, other):
        me = peer.xuid
        if self.store.relation(me, other) != INCOMING:
            return
        mine, theirs = self.store.get(me, True), self.store.get(other, True)
        if len(mine["friends"]) >= MAX_FRIENDS or len(theirs["friends"]) >= MAX_FRIENDS:
            self.tell(peer, "A friends list is full (%d)" % MAX_FRIENDS)
            return
        self.store.befriend(me, other)
        log.info("friends #%d %r and %r are friends", peer.id, peer.name, self.store.name(other))
        self.tell(peer, "You and %s are friends" % self.store.name(other))
        self.notice(other, N_ACCEPTED, me)
        self.push(me)
        self.push(other)

    def decline(self, peer, other):
        if self.store.relation(peer.xuid, other) in (INCOMING, OUTGOING):
            self.store.unlink(peer.xuid, other)
            self.push(peer.xuid)
            self.push(other)

    def remove(self, peer, other):
        if self.store.relation(peer.xuid, other) == FRIEND:
            self.store.unlink(peer.xuid, other)
            log.info("friends #%d %r removed %r", peer.id, peer.name, self.store.name(other))
            self.tell(peer, "%s is no longer your friend" % self.store.name(other))
            self.push(peer.xuid)
            self.push(other)

    def block(self, peer, other, name):
        me = peer.xuid
        if not other or other == me:
            return
        p = self.store.get(me, True)
        if len(p["blocked"]) >= MAX_BLOCKED and self.store.hx(other) not in p["blocked"]:
            self.tell(peer, "You blocked %d players already" % MAX_BLOCKED)
            return
        if name and not self.store.get(other):
            self.store.seen(other, name[:15])
        self.store.block(me, other)
        log.info("friends #%d %r blocked %r", peer.id, peer.name, self.store.name(other))
        self.tell(peer, "%s is blocked" % self.store.name(other))
        self.push(me)
        self.push(other)

    def unblock(self, peer, other):
        if self.store.unblock(peer.xuid, other):
            self.tell(peer, "%s is no longer blocked" % self.store.name(other))
            self.push(peer.xuid)

    def search(self, peer, request, query):
        """Players whose gamertag contains the query: online ones first."""
        q = query.strip().lower()
        found = {}
        if q:
            for p in self.server.peers.values():
                if p.xuid != peer.xuid and q in p.name.lower():
                    found.setdefault(p.xuid, (p.name, 1))
            if len(q) >= 2 or len(found) < MAX_RESULTS:
                for k, p in self.store.players.items():
                    x = int(k, 16)
                    if p["name"] and q in p["name"].lower() and x != peer.xuid:
                        found.setdefault(x, (p["name"], 0))
        rows = sorted(found.items(), key=lambda kv: (kv[1][0].lower() != q, -kv[1][1],
                                                     kv[1][0].lower()))[:MAX_RESULTS]
        body = struct.pack(">BHB", R_RESULTS, request, len(rows))
        for x, (name, online) in rows:
            body += struct.pack(">QBB", x, online, self.store.relation(peer.xuid, x)) + text8(name)
        self.server.send(peer.addr, KIND, body)

    def status(self):
        players = self.store.players.values()
        return {"friends": {"players": len(self.store.players),
                            "friendships": sum(len(p["friends"]) for p in players) // 2,
                            "requests": sum(len(p["out"]) for p in players)}}
