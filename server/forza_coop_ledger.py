"""The co-op campaign's ledger (#309, milestone 1 phase B; docs/coop_campaign.md).

A Co-op Festival campaign round (forza_campaign.py) runs as an online race, which writes no
career state. The server keeps what the career needs instead, and the player's launcher
applies it to the save after the session (launcher/forza_launcher/coop_grant.py):

- the round, when it starts (forza_world.Worlds.next_round, the event round's own prefetch):
  the world session, the event, the player that asked for it (/event, the world's host) and
  the humans in the world at that moment;
- each human's place: its own report (User.WriteRaceStatsEx: place of maxPlaces), and the
  host's XSessionWriteStats for the session (forza_stats: relative score = cars - place);
- which results are the career's: milestone 1 grants only the requester's (the host's own
  career; GRANT), only when the report and the host's write agree (or the host wrote none) and
  the place is within the field.

The launcher asks for its player's pending results and confirms the ones it applied:

    GET  /coop/results           {"results": [{"id", "event", "code", "place", "places",
                                 "at"}]} for the player of X-Xuid / X-Friends-Key (the same
                                 proof as the shader list upload: the friends key the XUID was
                                 bound to at a HELLO)
    POST /coop/results/applied   {"ids": [...]} -> {"applied": n}: these are not sent again

The launcher applies a result only when it improves the save's place for that event, so a
result applied twice (a lost confirmation) changes nothing. Kept in DATA_DIR/coop_ledger.json
(atomic writes): rounds for KEEP_DAYS, at most MAX_ROUNDS, a player's pending results at most
MAX_PENDING (the newest).
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
ROUND_WINDOW = 20 * 60.0   # seconds after a round's start in which its results are taken
KEEP_DAYS = 30
MAX_ROUNDS = 2000
MAX_PENDING = 50
MAX_IDS = 100              # ids in one confirmation
GRANT = "requester"        # whose result goes to the career (milestone 1: the host's own)


def hx(xuid):
    return "%016X" % xuid


class Ledger:
    def __init__(self, path=None):
        self.path = path
        self.rounds = {}  # id -> round
        self.lock = threading.Lock()

    # --- the file ---

    def load(self):
        if not self.path:
            return
        try:
            with open(self.path) as f:
                data = json.load(f)
            self.rounds = {r["id"]: r for r in data.get("rounds", []) if isinstance(r, dict)}
            self.expire()
            log.info("co-op ledger: %d round(s) from %s", len(self.rounds), self.path)
        except FileNotFoundError:
            pass
        except (OSError, ValueError, KeyError, TypeError) as e:
            log.warning("cannot read %s: %s", self.path, e)

    def save(self):
        if not self.path:
            return
        try:
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"version": 1, "rounds": list(self.rounds.values())}, f)
            os.replace(tmp, self.path)
        except OSError as e:
            log.warning("cannot write %s: %s", self.path, e)

    def expire(self, now=None):
        now = time.time() if now is None else now
        old = [k for k, r in self.rounds.items()
               if now - r["started"] > KEEP_DAYS * 86400 or self.settled(r)]
        for k in old:
            del self.rounds[k]
        if len(self.rounds) > MAX_ROUNDS:
            for k in sorted(self.rounds, key=lambda k: self.rounds[k]["started"])[:len(self.rounds) - MAX_ROUNDS]:
                del self.rounds[k]

    @staticmethod
    def grantees(r):
        return [r["by"]] if GRANT == "requester" else list(r["members"])

    def settled(self, r):
        """Every grantee's result was applied (or the round's window passed without one)."""
        late = time.time() - r["started"] > ROUND_WINDOW
        return all(x in r["applied"] or (late and x not in r["results"]) for x in self.grantees(r))

    # --- the game's side ---

    def start_round(self, sid, event, by_xuid, by_name, members, now=None):
        """A campaign round started (its race loads). Returns its id."""
        now = time.time() if now is None else now
        rid = hashlib.sha256(b"%s|%d|%f" % (sid, event.id, now)).hexdigest()[:16]
        with self.lock:
            self.rounds[rid] = {
                "id": rid, "sid": sid.hex(), "event": event.id, "code": event.code,
                "cars": event.drivers + 1, "by": hx(by_xuid), "by_name": by_name,
                "members": {hx(x): n for x, n in members.items()}, "started": now,
                "results": {}, "host": {}, "applied": []}
            self.expire(now)
            self.save()
        log.info("co-op ledger: round %s: %s (event %d) in world %s, asked by %s, %d human(s)",
                 rid, event.code, event.id, sid.hex(), by_name, len(members))
        return rid

    def current(self, member, now):
        """The newest round of the last ROUND_WINDOW seconds this player raced in."""
        found = [r for r in self.rounds.values()
                 if member in r["members"] and now - r["started"] <= ROUND_WINDOW]
        return max(found, key=lambda r: r["started"]) if found else None

    def report(self, xuid, place, places, finished, now=None):
        """User.WriteRaceStatsEx of a player: its place. Returns the round id or None."""
        now = time.time() if now is None else now
        with self.lock:
            r = self.current(hx(xuid), now)
            if r is None:
                return None
            r["results"][hx(xuid)] = {"place": place, "places": places, "finished": finished,
                                      "at": now}
            self.save()
        log.info("co-op ledger: round %s (%s): %s place %d/%d%s", r["id"], r["code"],
                 r["members"].get(hx(xuid)) or hx(xuid), place, places,
                 "" if finished else " (not finished)")
        return r["id"]

    def host_score(self, writer, session, target, score, now=None):
        """XSessionWriteStats' relative score for a player (forza_stats.on_skill_write): the
        host writes every human's cars - place at the end of the race. The session is the
        world's, and the game writes again when the world's next free roam ends (scores of
        the humans only, every team 0): the first write after the round started is the
        race's, later ones are kept out."""
        now = time.time() if now is None else now
        with self.lock:
            found = [r for r in self.rounds.values() if int(r["sid"], 16) == session
                     and hx(target) in r["members"] and 0 <= now - r["started"] <= ROUND_WINDOW]
            if not found:
                return
            r = max(found, key=lambda r: r["started"])
            if hx(target) in r["host"]:
                return
            r["host"][hx(target)] = {"score": score, "writer": hx(writer), "at": now}
            self.save()

    # --- the launcher's side ---

    def check(self, r, x):
        """The grantable result of player x in round r, or the reason it is not."""
        res = r["results"].get(x)
        if res is None:
            return "no report"
        if not res["finished"]:
            return "not finished"
        places = res["places"] or r["cars"]
        if not 1 <= res["place"] <= places or places > 8:
            return "place %d/%d" % (res["place"], places)
        host = r["host"].get(x)
        if host is not None and places - host["score"] != res["place"]:
            return "the host wrote place %d" % (places - host["score"])
        return None

    def pending(self, xuid):
        x = hx(xuid)
        out = []
        with self.lock:
            for r in self.rounds.values():
                if x not in self.grantees(r) or x in r["applied"]:
                    continue
                why = self.check(r, x)
                if why is None:
                    res = r["results"][x]
                    out.append({"id": r["id"], "event": r["event"], "code": r["code"],
                                "place": res["place"], "places": res["places"] or r["cars"],
                                "at": int(res["at"])})
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
                self.save()
        if n:
            log.info("co-op ledger: %s applied %d result(s)", x, n)
        return n


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
