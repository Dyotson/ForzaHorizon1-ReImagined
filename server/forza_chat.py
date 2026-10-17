"""Text chat on the Forza Live server (#92, step 2): the session or party a player is in,
the Horizon World, and direct messages between friends.

The server only relays: a message goes to the players online now and is gone. For
moderation it keeps the last RING messages in memory (time, sender, channel, recipient,
text; never written to disk), which the admin reads at /admin/chat. Admin actions (with
the admin token, see forza_live_lsp.is_admin): /admin/mute?who=<gamertag|XUID>&minutes=N
(0 lifts it) stops a player's messages, /admin/kick?who=...&minutes=N drops the player and
refuses its sign-in for N minutes (a ban; 0 lifts it). Mutes and bans are kept in
DATA_DIR/moderation.json until they run out. Players mute others for themselves (MUTE;
kept in friends.json with their friends); a player's blocks (forza_friends.py) hide its
messages too, both ways.

Games say they have chat with HELLO features 0x04 (FEATURE); games without it neither send
nor get messages.

Channels: SESSION, the players in a session with the sender (the game reports the ids of
the sessions it hosts or joined, Horizon World sessions excepted); WORLD, the players in the
sender's Horizon World (what matchmaking knows); DIRECT, one friend.

Message CHAT (UDP kind 23), body op u8 then (big endian; text u16 length + UTF-8):
  client -> server
    1 SESSIONS count u8, session ids u64 (at most 4; sent when they change and every 10 s)
    2 SAY      channel u8, friend's XUID u64 (DIRECT; else 0), text
    3 MUTE     xuid u64, on u8
  server -> client
    16 MESSAGE channel u8, sender XUID u64, sender gamertag (u8 length + UTF-8), text
    17 NOTICE  text
    18 MUTED   count u16, XUIDs u64 (the players this one muted; after every change and on
               request: SESSIONS with count 0xFF asks)
"""

import collections
import json
import logging
import os
import struct
import unicodedata
import time

import forza_friends as friends

log = logging.getLogger("forza-live")

KIND = 23
FEATURE = 0x04
SESSIONS, SAY, MUTE = 1, 2, 3
R_MESSAGE, R_NOTICE, R_MUTED = 16, 17, 18
SESSION, WORLD, DIRECT = 1, 2, 3
CHANNELS = {SESSION: "session", WORLD: "world", DIRECT: "direct"}

MAX_CHARS = 200
MAX_BYTES = 800
RATE = (0.5, 5)  # messages per second and burst, per player
RING = 200
MAX_SESSIONS = 4
MAX_MUTED = 200
FILE = "moderation.json"


def text16(s):
    b = s.encode()
    return struct.pack(">H", len(b)) + b


def clean(text):
    """The text as it is relayed: no control characters, one line, at most MAX_CHARS."""
    # Control and format characters (C0/C1, bidi overrides like U+202E, zero-width) become
    # spaces, so a message can't hide or reverse text in other players' overlays.
    text = "".join(" " if unicodedata.category(c) in ("Cc", "Cf", "Zl", "Zp") else c for c in text)
    return " ".join(text.split())[:MAX_CHARS]


class Chat:
    def __init__(self, server, friend_store, path=None):
        self.server = server
        self.store = friend_store
        self.path = path
        self.ring = collections.deque(maxlen=RING)
        self.mutes = {}  # xuid -> until (wall time): the admin's
        self.bans = {}   # xuid -> until
        self.buckets = {}

    # --- moderation state ---

    def load(self):
        if not self.path:
            return
        try:
            with open(self.path) as f:
                data = json.load(f)
            now = time.time()
            self.mutes = {int(k, 16): v for k, v in data.get("mutes", {}).items() if v > now}
            self.bans = {int(k, 16): v for k, v in data.get("bans", {}).items() if v > now}
        except FileNotFoundError:
            pass
        except (OSError, ValueError, AttributeError, TypeError) as e:
            log.warning("cannot read %s: %s", self.path, e)

    def save(self):
        if not self.path:
            return
        try:
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"mutes": {"%016X" % k: v for k, v in self.mutes.items()},
                           "bans": {"%016X" % k: v for k, v in self.bans.items()}}, f)
            os.replace(tmp, self.path)
        except OSError as e:
            log.warning("cannot write %s: %s", self.path, e)

    def banned(self, xuid):
        until = self.bans.get(xuid)
        if until and until <= time.time():
            del self.bans[xuid]
            until = None
        return until

    def admin_muted(self, xuid):
        until = self.mutes.get(xuid)
        if until and until <= time.time():
            del self.mutes[xuid]
            until = None
        return until

    def find(self, who):
        """XUIDs a gamertag or hex XUID names (online players and players the server
        knows)."""
        who = who.strip()
        try:
            if len(who) == 16:
                return [int(who, 16)]
        except ValueError:
            pass
        found = [p.xuid for p in self.server.peers.values() if p.name.lower() == who.lower()]
        found += [x for x in self.store.find(who) if x not in found]
        return found

    def admin(self, action, who, minutes):
        """/admin/mute and /admin/kick: {"players": [...]}, or None (no such player)."""
        xuids = self.find(who)
        if not xuids:
            return None
        until = time.time() + minutes * 60 if minutes > 0 else 0
        out = []
        for x in xuids:
            table = self.mutes if action == "mute" else self.bans
            if until:
                table[x] = until
            else:
                table.pop(x, None)
            dropped = 0
            for p in self.server.peers.values():
                if p.xuid != x:
                    continue
                if action == "mute" and p.features & FEATURE:
                    self.notice(p, "The server admin muted you for %d min" % minutes if until
                                else "The server admin lifted your chat mute")
                elif action == "kick" and until:
                    self.notice(p, "The server admin removed you for %d min" % minutes)
            if action == "kick" and until:
                for p in [p for p in self.server.peers.values() if p.xuid == x]:
                    self.server.drop(p, "kicked by the admin")
                    dropped += 1
            log.info("admin: %s %016X %r for %d min%s", action, x, self.store.name(x), minutes,
                     " (%d dropped)" % dropped if dropped else "")
            out.append({"xuid": "%016X" % x, "gamertag": self.store.name(x),
                        "until": int(until), "dropped": dropped})
        self.save()
        return {"players": out}

    def moderation(self):
        now = time.time()
        return {"messages": list(self.ring),
                "mutes": {self.store.name(k): int(v - now) for k, v in self.mutes.items() if v > now},
                "bans": {self.store.name(k): int(v - now) for k, v in self.bans.items() if v > now}}

    # --- messages ---

    def notice(self, peer, text):
        self.server.send(peer.addr, KIND, bytes([R_NOTICE]) + text16(text))

    def muted_list(self, xuid):
        p = self.store.get(xuid)
        return [int(k, 16) for k in (p.get("muted", {}) if p else {})]

    def send_muted(self, peer):
        xs = self.muted_list(peer.xuid)
        self.server.send(peer.addr, KIND, struct.pack(">BH", R_MUTED, len(xs)) +
                         b"".join(struct.pack(">Q", x) for x in xs))

    def hides(self, reader, sender):
        """The reader does not get the sender's messages (blocked either way, or muted)."""
        if self.store.blocks(reader, sender) or self.store.blocks(sender, reader):
            return True
        p = self.store.get(reader)
        return bool(p) and self.store.hx(sender) in p.get("muted", {})

    def limited(self, xuid):
        rate, burst = RATE
        now = time.monotonic()
        tokens, last = self.buckets.get(xuid, (burst, now))
        tokens = min(burst, tokens + (now - last) * rate)
        ok = tokens >= 1
        self.buckets[xuid] = (tokens - 1 if ok else tokens, now)
        if len(self.buckets) > 20000:
            self.buckets = {k: v for k, v in self.buckets.items() if now - v[1] < 60}
        return not ok

    def chatters(self):
        return [p for p in self.server.peers.values()
                if p.features & FEATURE and getattr(p, "friends_ok", False)]

    def on_message(self, peer, body):
        if not peer.features & FEATURE:
            return
        op = body[0]
        if op == SESSIONS:
            count = body[1]
            if count == 0xFF:
                self.send_muted(peer)
                return
            ids = [struct.unpack_from(">Q", body, 2 + 8 * i)[0] for i in range(min(count, MAX_SESSIONS))]
            peer.chat_sessions = {i for i in ids if i}
        elif op == MUTE:
            if not getattr(peer, "friends_ok", False):
                return
            other, on = struct.unpack_from(">QB", body, 1)
            p = self.store.get(peer.xuid, True)
            muted = p.setdefault("muted", {})
            k = self.store.hx(other)
            if on and k not in muted and len(muted) < MAX_MUTED and other != peer.xuid:
                muted[k] = time.time()
            elif not on:
                muted.pop(k, None)
            self.store.dirty = True
            self.send_muted(peer)
        elif op == SAY:
            channel, target = struct.unpack_from(">BQ", body, 1)
            (size,) = struct.unpack_from(">H", body, 10)
            raw = body[12:12 + size]
            if size > MAX_BYTES or len(raw) != size:
                return
            self.say(peer, channel, target, clean(raw.decode("utf-8", "replace")))

    def recipients(self, peer, channel, target):
        """The players a message goes to, or a reason it goes nowhere."""
        others = [p for p in self.chatters() if p.xuid != peer.xuid]
        if channel == DIRECT:
            if self.store.relation(peer.xuid, target) != friends.FRIEND:
                return "You can only message your friends"
            to = [p for p in others if p.xuid == target]
            return to or "%s is offline" % self.store.name(target)
        if channel == WORLD:
            mine = self.server.world_of_peer(peer.id)
            if mine is None:
                return "You are not in a Horizon World"
            to = [p for p in others if p.room == peer.room
                  and self.server.world_of_peer(p.id) == mine]
            return to or "No one else in your Horizon World has chat"
        if channel == SESSION:
            mine = set(getattr(peer, "chat_sessions", ()))
            world = self.server.world_of_peer(peer.id)
            if world is not None:
                mine.discard(struct.unpack(">Q", world)[0])
            if not mine:
                return "You are not in a session"
            to = [p for p in others if p.room == peer.room
                  and mine & set(getattr(p, "chat_sessions", ()))]
            return to or "No one else in your session has chat"
        return "Unknown channel"

    def say(self, peer, channel, target, text):
        if not text:
            return
        if not getattr(peer, "friends_ok", False):
            self.notice(peer, "Chat is off: this XUID belongs to another installation")
            return
        until = self.admin_muted(peer.xuid)
        if until:
            self.notice(peer, "The server admin muted you (%d min left)" %
                        max(1, int((until - time.time()) / 60 + 0.99)))
            return
        if self.limited(peer.xuid):
            self.notice(peer, "Slow down: at most %d messages in %d s" %
                        (RATE[1], int(RATE[1] / RATE[0])))
            return
        to = self.recipients(peer, channel, target)
        if isinstance(to, str):
            self.notice(peer, to)
            return
        delivered = [p for p in to if not self.hides(p.xuid, peer.xuid)]
        name = peer.name.encode()[:15]
        msg = struct.pack(">BBQB", R_MESSAGE, channel, peer.xuid, len(name)) + name + text16(text)
        for p in delivered:
            self.server.send(p.addr, KIND, msg)
        self.ring.append({"time": int(time.time()), "from": peer.name,
                          "xuid": "%016X" % peer.xuid, "channel": CHANNELS.get(channel),
                          "to": [p.name for p in to], "text": text})
        log.debug("chat #%d %r %s -> %d", peer.id, peer.name, CHANNELS.get(channel),
                  len(delivered))
