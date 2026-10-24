"""The co-op campaign's ledger (#309, milestone 1 phase B; docs/coop_campaign.md).

A Co-op Festival campaign round (forza_campaign.py) runs as an online race, which writes no
career state. The server keeps what the career needs instead, and the player's launcher
applies it to the save after the session (launcher/forza_launcher/coop_grant.py):

- the round, from its start (forza_world.Worlds.next_round: the event round's own prefetch,
  asked by the world's host with its own friends key) until the world's next round starts
  (close): the world session, the event, its grid (the MaxPlayers it was served with), the
  player that asked for it (/event, the world's host) and the humans in the world;
- each human's place: the first report of its own (User.WriteRaceStatsEx with its own
  friends key, forza_live_lsp.caller_trusted, for this world's session id, while the round is
  open), and the requester's own XSessionWriteStats for the session (forza_stats: relative
  score = cars - place; first write after the start, from the requester's game only);
- which results are the career's: milestone 1 grants only the requester's (the host's own
  career; GRANT), when its report and its game's write agree (or there is no write).

The launcher asks for its player's pending results and confirms the ones it settled:

    GET  /coop/results           {"results": [{"id", "event", "code", "place", "places",
                                 "at"}]} for the player of X-Xuid / X-Friends-Key (the same
                                 proof as the shader list upload)
    POST /coop/results/applied   {"ids": [...]} -> {"applied": n}: these are not sent again

The launcher applies a result only to the save of the session it was earned in, only for an
event that save has unlocked, and only when it improves the save's place, so a result applied
twice changes nothing. Kept in DATA_DIR/coop_ledger.json (written at most every SAVE_EVERY
seconds, from the server's tick): rounds for KEEP_DAYS, at most MAX_ROUNDS_PER_PLAYER asked by
one player (its oldest go first) and MAX_ROUNDS in all.
"""

import hashlib
import json
import logging
import os
import threading
import time
import urllib.parse

log = logging.getLogger("forza-live")

FILE = "coop_ledger.json"
OPEN_LIMIT = 30 * 60.0      # a round left open this long (its world gone) takes no more reports
KEEP_DAYS = 30
MAX_ROUNDS = 2000
MAX_ROUNDS_PER_PLAYER = 20  # rounds one player asked for and has not settled
MAX_PENDING = 50
MAX_IDS = 100               # ids in one confirmation
SAVE_EVERY = 2.0
GRANT = "requester"         # whose result goes to the career (milestone 1: the host's own)


def hx(xuid):
    return "%016X" % xuid


class Ledger:
    def __init__(self, path=None):
        self.path = path
        self.rounds = {}  # id -> round
        self.lock = threading.Lock()
        self.dirty = False
        self.saved = 0.0

    # --- the file ---

    def load(self):
        if not self.path:
            return
        try:
            with open(self.path) as f:
                data = json.load(f)
            self.rounds = {r["id"]: r for r in data.get("rounds", []) if isinstance(r, dict)}
            for r in self.rounds.values():  # version 1 kept no open flag: closed
                r.setdefault("open", False)
            self.expire()
            log.info("co-op ledger: %d round(s) from %s", len(self.rounds), self.path)
        except FileNotFoundError:
            pass
        except (OSError, ValueError, KeyError, TypeError) as e:
            log.warning("cannot read %s: %s", self.path, e)

    def save(self):
        """Writes the ledger. It stays dirty until the file is replaced, so a failed write is
        tried again at the next tick and at shutdown."""
        self.saved = time.monotonic()
        if not self.path:
            self.dirty = False
            return
        try:
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"version": 2, "rounds": list(self.rounds.values())}, f)
            os.replace(tmp, self.path)
        except OSError as e:
            log.warning("cannot write %s: %s", self.path, e)
            return
        self.dirty = False

    def maybe_save(self):
        """From the server's tick: the changes of the last SAVE_EVERY seconds, in one write."""
        if self.dirty and time.monotonic() - self.saved >= SAVE_EVERY:
            with self.lock:
                self.save()

    def expire(self, now=None):
        now = time.time() if now is None else now
        for k in [k for k, r in self.rounds.items()
                  if now - r["started"] > KEEP_DAYS * 86400 or self.settled(r, now)]:
            del self.rounds[k]
        by = {}
        for r in sorted(self.rounds.values(), key=lambda r: r["started"], reverse=True):
            by.setdefault(r["by"], []).append(r["id"])
        for ids in by.values():
            for k in ids[MAX_ROUNDS_PER_PLAYER:]:
                del self.rounds[k]
        if len(self.rounds) > MAX_ROUNDS:
            for k in sorted(self.rounds, key=lambda k: self.rounds[k]["started"])[:len(self.rounds) - MAX_ROUNDS]:
                del self.rounds[k]

    @staticmethod
    def grantees(r):
        return [r["by"]] if GRANT == "requester" else list(r["members"])

    def settled(self, r, now):
        """Every grantee's result was confirmed, or the round closed without one."""
        closed = not r["open"] or now - r["started"] > OPEN_LIMIT
        return all(x in r["applied"] or (closed and x not in r["results"]) for x in self.grantees(r))

    # --- the game's side ---

    def open_round(self, sid):
        return next((r for r in self.rounds.values() if r["open"] and r["sid"] == sid.hex()), None)

    def start_round(self, sid, event, by_xuid, by_name, members, now=None):
        """A campaign round started (its race loads). Returns its id."""
        now = time.time() if now is None else now
        rid = hashlib.sha256(b"%s|%d|%f" % (sid, event.id, now)).hexdigest()[:16]
        with self.lock:
            for r in self.rounds.values():  # a round of this world that was never closed
                if r["sid"] == sid.hex():
                    r["open"] = False
            self.rounds[rid] = {
                "id": rid, "sid": sid.hex(), "event": event.id, "code": event.code,
                "cars": event.drivers + 1, "by": hx(by_xuid), "by_name": by_name,
                "members": {hx(x): n for x, n in members.items()}, "started": now, "open": True,
                "results": {}, "host": {}, "applied": []}
            self.expire(now)
            self.dirty = True
        log.info("co-op ledger: round %s: %s (event %d) in world %s, asked by %s, %d human(s)",
                 rid, event.code, event.id, sid.hex(), by_name, len(members))
        return rid

    def close_round(self, sid):
        """The world's next round started: its campaign round takes no more reports."""
        with self.lock:
            for r in self.rounds.values():
                if r["open"] and r["sid"] == sid.hex():
                    r["open"] = False
                    self.dirty = True
                    log.info("co-op ledger: round %s (%s) closed with %d result(s)", r["id"],
                             r["code"], len(r["results"]))

    def report(self, xuid, session, place, finished, now=None):
        """A player's own User.WriteRaceStatsEx (verified by the caller) in world `session`
        (8 bytes): its place. The first one counts. Returns the round id or None."""
        now = time.time() if now is None else now
        x = hx(xuid)
        with self.lock:
            r = self.open_round(session)
            if r is None or x not in r["members"] or now - r["started"] > OPEN_LIMIT:
                return None
            if x in r["results"]:
                return r["id"]
            r["results"][x] = {"place": place, "finished": finished, "at": now}
            self.dirty = True
        log.info("co-op ledger: round %s (%s): %s place %d/%d%s", r["id"], r["code"],
                 r["members"].get(x) or x, place, r["cars"], "" if finished else " (not finished)")
        return r["id"]

    def host_score(self, writer, session, target, score, now=None):
        """XSessionWriteStats' relative score for a player (forza_stats.on_skill_write): the
        requester's game writes every human's cars - place at the end of the race. Only its
        own writes count, and the first after the round's start: the game writes again for the
        world's session when its next free roam ends (scores of the humans only)."""
        now = time.time() if now is None else now
        with self.lock:
            r = next((r for r in self.rounds.values() if r["open"]
                      and int(r["sid"], 16) == session), None)
            if r is None or hx(writer) != r["by"] or hx(target) not in r["members"]:
                return
            if hx(target) in r["host"] or now - r["started"] > OPEN_LIMIT:
                return
            r["host"][hx(target)] = {"score": score, "at": now}
            self.dirty = True

    # --- the launcher's side ---

    def check(self, r, x):
        """None for a grantable result of player x in round r, or the reason it is not."""
        res = r["results"].get(x)
        if res is None:
            return "no report"
        if not res["finished"]:
            return "not finished"
        if not 1 <= res["place"] <= r["cars"]:
            return "place %d of %d" % (res["place"], r["cars"])
        host = r["host"].get(x)
        if host is not None and r["cars"] - host["score"] != res["place"]:
            return "its game wrote place %d" % (r["cars"] - host["score"])
        return None

    def pending(self, xuid):
        x = hx(xuid)
        out = []
        with self.lock:
            for r in self.rounds.values():
                if x not in self.grantees(r) or x in r["applied"]:
                    continue
                if self.check(r, x) is None:
                    res = r["results"][x]
                    out.append({"id": r["id"], "event": r["event"], "code": r["code"],
                                "place": res["place"], "places": r["cars"], "at": int(res["at"])})
        return sorted(out, key=lambda e: e["at"])[-MAX_PENDING:]

    def applied(self, xuid, ids):
        x = hx(xuid)
        n = 0
        with self.lock:
            for rid in ids[:MAX_IDS]:
                r = self.rounds.get(rid) if isinstance(rid, str) else None
                if r is not None and x in self.grantees(r) and x not in r["applied"]:
                    r["applied"].append(x)
                    n += 1
            if n:
                self.expire()
                self.dirty = True
        if n:
            log.info("co-op ledger: %s settled %d result(s)", x, n)
        return n

    def unsettled(self, xuid):
        """Rounds this player asked for that are not settled (its /event is refused beyond
        MAX_ROUNDS_PER_PLAYER)."""
        x = hx(xuid)
        now = time.time()
        with self.lock:
            return sum(1 for r in self.rounds.values() if r["by"] == x and not self.settled(r, now))


LEDGER = Ledger()


def configure(data_dir):
    LEDGER.path = os.path.join(data_dir, FILE)
    LEDGER.load()


def handle(server, method, path, headers, body, authenticate):
    """(status, content type, body) for /coop/...; None for another path. authenticate(store,
    headers) -> XUID or None (forza_shader_lists.authenticate)."""
    url = urllib.parse.urlsplit(path)
    if url.path not in ("/coop/results", "/coop/results/applied"):
        return None

    def reply(status, obj):
        return status, "application/json", json.dumps(obj).encode()
    xuid = authenticate(server.friends.store, headers)
    if xuid is None:
        return reply(403, {"error": "unknown player"})
    if url.path == "/coop/results" and method == "GET":
        return reply(200, {"results": LEDGER.pending(xuid)})
    if url.path == "/coop/results/applied" and method == "POST":
        try:
            ids = json.loads(body or b"{}").get("ids", [])
        except (ValueError, AttributeError):
            return reply(400, {"error": "body"})
        if not isinstance(ids, list):
            return reply(400, {"error": "ids"})
        return reply(200, {"applied": LEDGER.applied(xuid, ids)})
    return reply(405, {"error": "method"})
