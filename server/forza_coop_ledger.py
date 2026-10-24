"""The co-op campaign's ledger (#309, milestones 1 and 2; docs/coop_campaign.md).

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
- which results are a career's (check): every human's own (milestone 2: the host's and each
  guest's), finished, on the grid, and agreeing with the requester's game's write. The
  requester's own place may stand without that write (milestone 1); a guest's needs it, so a
  guest's place is always confirmed by a second console. A street race (--coop-street) gives
  none: the online race flow turns traffic off, so it is not the career's street race.

The launcher asks for its player's pending results and confirms the ones it settled:

    GET  /coop/results           {"results": [{"id", "event", "code", "place", "places",
                                 "at", "until", "host", "winnings"}]} for the player of X-Xuid /
                                 X-Friends-Key (the same proof as the shader list upload);
                                 until: when the server drops it unapplied, host: the
                                 world's host who started it, winnings: the player's
                                 onlineWinnings before that race paid and at its next race
                                 (null until there is one): what the online race itself paid,
                                 winnings_at: when that next race was (any save of the PC
                                 shares the identity: the launcher uses it only for its own)
    POST /coop/results/applied   {"ids": [...]} -> {"applied": n}: these are not sent again

The launcher applies a result only to the save of the session it was earned in, only for an
event that save has unlocked, and only when it improves the save's place, so a result applied
twice changes nothing. A result waits on the server until it is confirmed (the launcher does not
confirm one for an event its save has not unlocked yet), for KEEP_DAYS at most, and at most
MAX_WAITING per player (grantable results of closed rounds; the ones earned as a guest go
first, then the oldest). The host's /event is refused while MAX_WAITING results of rounds it
asked for wait, so its own rounds never drop its own results. Kept in DATA_DIR/coop_ledger.json (written at most every SAVE_EVERY seconds, from the
server's tick), with MAX_ROUNDS rounds in all.
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
MAX_WAITING = 20            # results of one player waiting for its launcher
MAX_PENDING = 50
MAX_IDS = 100               # ids in one confirmation
SAVE_EVERY = 2.0


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
                r.setdefault("dropped", [])
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
                json.dump({"version": 3, "rounds": list(self.rounds.values())}, f)
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
        by = {}  # each player's grantable results of closed rounds: beyond MAX_WAITING they drop
        for r in self.rounds.values():
            for x in r["members"]:
                if self.capped(r, x, now):
                    by.setdefault(x, []).append(r)
        for x, rounds in by.items():
            # kept first: the rounds the player asked for, then the newest
            rounds.sort(key=lambda r: (r["by"] == x, r["started"]), reverse=True)
            for r in rounds[MAX_WAITING:]:
                r["dropped"].append(x)
                log.info("co-op ledger: round %s (%s): %s's result dropped unapplied (%d newer "
                         "ones wait)", r["id"], r["code"], r["members"].get(x) or x, MAX_WAITING)
        for k in [k for k, r in self.rounds.items()
                  if now - r["started"] > KEEP_DAYS * 86400 or self.settled(r, now)]:
            del self.rounds[k]
        if len(self.rounds) > MAX_ROUNDS:
            for k in sorted(self.rounds, key=lambda k: self.rounds[k]["started"])[:len(self.rounds) - MAX_ROUNDS]:
                del self.rounds[k]

    @staticmethod
    def closed(r, now):
        return not r["open"] or now - r["started"] > OPEN_LIMIT

    def waiting(self, r, x, now):
        """Player x has a result in round r that may still go to its career: reported, not
        confirmed or dropped, and grantable (or the round is still open, so its game's write
        may still come)."""
        return (x in r["results"] and x not in r["applied"] and x not in r["dropped"]
                and not (self.closed(r, now) and self.check(r, x) is not None))

    def capped(self, r, x, now):
        """Player x's result in round r counts toward its MAX_WAITING: the round is closed and
        the result is grantable and not settled. A result of a round still open does not count
        yet (it may never become grantable: not finished, off the grid, no host write)."""
        return self.closed(r, now) and self.waiting(r, x, now)

    def settled(self, r, now):
        """The round is closed and no result of it waits."""
        return self.closed(r, now) and not any(self.waiting(r, x, now) for x in r["members"])

    # --- the game's side ---

    def open_round_of(self, xuid, now=None):
        """The id of the open campaign round the player is one of the humans of, or None."""
        now = time.time() if now is None else now
        x = hx(xuid)
        with self.lock:
            return next((r["id"] for r in self.rounds.values()
                         if not self.closed(r, now) and x in r["members"]), None)

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
                "cars": event.drivers + 1, "street": bool(getattr(event, "street", False)),
                "by": hx(by_xuid), "by_name": by_name,
                "members": {hx(x): n for x, n in members.items()}, "started": now, "open": True,
                "results": {}, "host": {}, "applied": [], "dropped": []}
            self.expire(now)
            self.dirty = True
        log.info("co-op ledger: round %s: %s (event %d) in world %s, asked by %s, %d human(s)",
                 rid, event.code, event.id, sid.hex(), by_name, len(members))
        return rid

    def close_round(self, sid):
        """The world's next round started: its campaign round takes no more reports (and is
        dropped if none of its results waits). Returns the rounds it closed (copies)."""
        with self.lock:
            closed = [r for r in self.rounds.values() if r["open"] and r["sid"] == sid.hex()]
            for r in closed:
                r["open"] = False
                log.info("co-op ledger: round %s (%s) closed with %d result(s)%s", r["id"],
                         r["code"], len(r["results"]), "".join(
                             "; %s %s" % (r["members"].get(x) or x, self.check(r, x) or "place %d/%d"
                                          % (r["results"][x]["place"], r["cars"]))
                             for x in r["results"]))
            if closed:
                self.expire()
                self.dirty = True
            return [dict(r) for r in closed]

    def race_written(self, xuid, winnings):
        """Any race's User.WriteRaceStatsEx from the player's own game: its onlineWinnings
        (written before that race pays) closes the online winnings of the player's previous
        campaign round. Call it before report()."""
        if type(winnings) is not int:
            return
        x = hx(xuid)
        with self.lock:
            for r in self.rounds.values():
                res = r["results"].get(x)
                if res is not None and "ow" in res and "ow_after" not in res:
                    res["ow_after"] = winnings
                    res["ow_at"] = time.time()
                    self.dirty = True

    def report(self, xuid, session, place, finished, now=None, winnings=None):
        """A player's own User.WriteRaceStatsEx (verified by the caller) in world `session`
        (8 bytes): its place, and its online winnings before this race pays (onlineWinnings).
        The first one counts. Returns the round id or None."""
        now = time.time() if now is None else now
        x = hx(xuid)
        with self.lock:
            r = self.open_round(session)
            if r is None or x not in r["members"] or now - r["started"] > OPEN_LIMIT:
                return None
            if x in r["results"]:
                return r["id"]
            r["results"][x] = {"place": place, "finished": finished, "at": now}
            if type(winnings) is int:
                r["results"][x]["ow"] = winnings
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
        """None for a grantable result of player x in round r, or the reason it is not. The
        requester's game writes every human's place: a guest's place needs that write to
        agree; the requester's own stands without it (its own game wrote both)."""
        res = r["results"].get(x)
        if res is None:
            return "no report"
        if r.get("street"):  # the online race flow turns traffic off: not the career's race
            return "a street race without traffic"
        if not res["finished"]:
            return "not finished"
        if not 1 <= res["place"] <= r["cars"]:
            return "place %d of %d" % (res["place"], r["cars"])
        host = r["host"].get(x)
        if host is None:
            return None if x == r["by"] else "no place from the host's game"
        if r["cars"] - host["score"] != res["place"]:
            return "%s game wrote place %d" % ("its" if x == r["by"] else "the host's",
                                               r["cars"] - host["score"])
        return None

    def pending(self, xuid):
        x = hx(xuid)
        out = []
        with self.lock:
            for r in self.rounds.values():
                if x not in r["members"] or x in r["applied"] or x in r["dropped"]:
                    continue
                if self.check(r, x) is None:
                    res = r["results"][x]
                    out.append({"id": r["id"], "event": r["event"], "code": r["code"],
                                "place": res["place"], "places": r["cars"], "at": int(res["at"]),
                                "until": int(r["started"] + KEEP_DAYS * 86400),
                                "host": r.get("by_name") or r["by"]})
                    if "ow" in res:  # [before the race paid, at the player's next race]
                        out[-1]["winnings"] = [res["ow"], res.get("ow_after")]
                        if "ow_at" in res:  # when: the launcher checks it was the same save
                            out[-1]["winnings_at"] = int(res["ow_at"])
        return sorted(out, key=lambda e: e["at"])[-MAX_PENDING:]

    def applied(self, xuid, ids):
        x = hx(xuid)
        n = 0
        with self.lock:
            for rid in ids[:MAX_IDS]:
                r = self.rounds.get(rid) if isinstance(rid, str) else None
                if (r is not None and x in r["results"] and x not in r["applied"]
                        and x not in r["dropped"]):
                    r["applied"].append(x)
                    n += 1
            if n:
                self.expire()
                self.dirty = True
        if n:
            log.info("co-op ledger: %s settled %d result(s)", x, n)
        return n

    def waiting_for(self, xuid):
        """This player's results of rounds it asked for that wait for its launcher (its
        /event is refused at MAX_WAITING, so that its own rounds never drop one; results it
        earned as a guest don't count: the cap drops those first)."""
        x = hx(xuid)
        now = time.time()
        with self.lock:
            return sum(1 for r in self.rounds.values() if r["by"] == x and self.capped(r, x, now))

    def refusal(self, xuid):
        """The /event notice at MAX_WAITING: why, and until when."""
        x = hx(xuid)
        now = time.time()
        with self.lock:
            mine = [r for r in self.rounds.values() if r["by"] == x and self.capped(r, x, now)]
        oldest = min((r["started"] for r in mine), default=now) + KEEP_DAYS * 86400
        return ("%d results of co-op events you started still wait for your launcher (an event "
                "your career has not unlocked keeps its result until it is unlocked). The oldest "
                "expires on %s; start more events after that, or after your launcher has applied "
                "some" % (len(mine), time.strftime("%d %b %Y", time.gmtime(oldest))))


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
