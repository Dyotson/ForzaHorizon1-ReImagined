"""Forza Live: Xbox LIVE stats (XSessionWriteStats, XUserReadStats) per player and view.

Forza Horizon's title (its SPA's XVC2 table) defines one stats view and no columns of its
own: the TrueSkill view 0xFFFF0000 (X_STATS_VIEW_SKILL_RANKED of game mode 0), whose
columns are the system's 61 skill, 62 games played, 63 mu, 64 sigma. At the end of an
online game it writes each player's X_PROPERTY_RELATIVE_SCORE (0x1000800A) and
X_PROPERTY_SESSION_TEAM (0x1000800B) to that view (XSessionWriteStats, one view per call),
and it reads the view for its party (XUserReadStats). Its leaderboards (events, speed
traps, Rivals, the 1000 Club) are title services instead (forza_scores.py).

The game sends the writes and reads to the server over UDP (forza_live_server.py, STATS):

  write: op 1, session id u64, XUID u64, view count u8, per view: view id u32, property
         count u8, per property: id u32, XUSER_DATA type u8, value (8 bytes as in the game)
  read:  op 2, request u32, XUID count u8, XUIDs u64..., spec count u8, per spec: view id
         u32, column count u8, column ids u16...
  rows (the answer, STATS_ROWS): request u32, view count u8, per view: view id u32, total
         rows u32, row count u8, per row: XUID u64, rank u32, rating i64, gamertag (u8
         length + bytes), column count u8, per column: id u16, type u8, value (8 bytes)

A skill view's writes are a game's result: the writes of one session are collected and,
SETTLE seconds after the last one, ranked by relative score (bigger is better, equal is a
draw; teams rank by their players' best score) and every player's mu and sigma updated
(TrueSkill, pairwise between adjacent places). Other views keep the last value written for
each property, read as the column with the property id's low 16 bits. Rows of players
nobody wrote have a new player's defaults (games 0, mu 3, sigma 1, skill 1). Kept in
DATA_DIR/stats.json, at most limits.MAX_PLAYERS players and MAX_VIEWS views each.
"""

import json
import logging
import math
import os
import struct
import time

import forza_limits as limits

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"
SETTLE = 10.0          # seconds after a session's last write before its game is rated
MAX_VIEWS = 16         # views per player
MAX_PROPERTIES = 64    # properties per view and write
MAX_PENDING = 256      # sessions waiting to be rated
MAX_ROWS_BYTES = 1400  # an answer fits one datagram

RELATIVE_SCORE, SESSION_TEAM = 0x1000800A, 0x1000800B
COL_SKILL, COL_GAMES, COL_MU, COL_SIGMA = 61, 62, 63, 64
TYPE_INT32, TYPE_INT64, TYPE_DOUBLE, TYPE_NULL = 1, 2, 3, 0xFF
MU0, SIGMA0 = 3.0, 1.0      # X_STATS_SKILL_MU_DEFAULT, X_STATS_SKILL_SIGMA_DEFAULT
BETA, TAU = SIGMA0 / 2, SIGMA0 / 100
SKILL_MIN, SKILL_MAX = 1, 50


def is_skill_view(view):
    return view >> 16 in (0xFFFF, 0xFFFE)  # ranked, standard


def skill_of(mu, sigma):
    """The 1..50 skill shown for a rating: the conservative mu - 3 sigma (0 for a new
    player) spread over 0..6."""
    return max(SKILL_MIN, min(SKILL_MAX, SKILL_MIN + round((mu - 3 * sigma) / 6 * 49)))


def _pdf(x):
    return math.exp(-x * x / 2) / math.sqrt(2 * math.pi)


def _cdf(x):
    return 0.5 * (1 + math.erf(x / math.sqrt(2)))


def rate(places):
    """New (mu, sigma) per player. places: [(place, [(key, mu, sigma)])], place 0 the
    winner, equal places a draw."""
    places = sorted(places, key=lambda p: p[0])
    teams = []
    for place, players in places:
        mu = sum(p[1] for p in players)
        var = sum(p[2] ** 2 + TAU ** 2 for p in players)
        teams.append([place, players, mu, var, 0.0, 0.0])  # dmu, dvar factor sums
    for i in range(len(teams) - 1):
        a, b = teams[i], teams[i + 1]
        c = math.sqrt(2 * BETA ** 2 * (len(a[1]) + len(b[1])) / 2 + a[3] + b[3])
        t = (a[2] - b[2]) / c
        if a[0] == b[0]:  # a draw: pull together
            v, w = -t * 0.5, 0.25
        else:
            denom = max(_cdf(t), 1e-12)
            v = _pdf(t) / denom
            w = v * (v + t)
        a[4] += v / c
        b[4] -= v / c
        a[5] += w / (c * c)
        b[5] += w / (c * c)
    out = {}
    for _, players, _, _, dmu, dvar in teams:
        for key, mu, sigma in players:
            var = sigma ** 2 + TAU ** 2
            new_mu = mu + var * dmu
            new_var = var * max(1 - var * dvar, 0.0001)
            out[key] = (new_mu, math.sqrt(new_var))
    return out


class Store:
    def __init__(self):
        self.players = {}   # xuid hex -> {"gamertag", "views": {view id str: view}}
        self.pending = {}   # (session id, view) -> {"at": monotonic, "writes": {xuid: (score, team)}}
        self.loaded = False

    def path(self):
        return os.path.join(DATA_DIR, "stats.json")

    def load(self):
        if self.loaded:
            return
        self.loaded = True
        try:
            with open(self.path()) as f:
                self.players = json.load(f).get("players", {})
        except (OSError, ValueError):
            pass

    def save(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        tmp = self.path() + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"players": self.players}, f, indent=1)
        os.replace(tmp, self.path())

    def view(self, xuid, view, gamertag=None, create=False):
        self.load()
        key = "%016X" % xuid
        p = self.players.get(key)
        if p is None:
            if not create or len(self.players) >= limits.MAX_PLAYERS:
                return None
            p = self.players[key] = {"gamertag": gamertag or key, "views": {}}
        if gamertag:
            p["gamertag"] = gamertag
        v = p["views"].get(str(view))
        if v is None and create and len(p["views"]) < MAX_VIEWS:
            v = p["views"][str(view)] = {"columns": {}}
            if is_skill_view(view):
                v.update(mu=MU0, sigma=SIGMA0, games=0)
        return v


STORE = Store()


def configure(data_dir):
    global DATA_DIR
    DATA_DIR = data_dir
    STORE.__init__()


def parse_write(body):
    """(session id, XUID, [(view, [(id, type, value bytes)])]) of a write (after the op)."""
    session, xuid = struct.unpack_from(">QQ", body, 0)
    pos = 17
    views = []
    for _ in range(min(body[16], MAX_VIEWS)):
        view, count = struct.unpack_from(">IB", body, pos)
        pos += 5
        props = []
        for _ in range(min(count, MAX_PROPERTIES)):
            pid, kind = struct.unpack_from(">IB", body, pos)
            value = body[pos + 5:pos + 13]
            if len(value) != 8:
                raise ValueError("short property")
            props.append((pid, kind, value))
            pos += 13
        views.append((view, props))
    return session, xuid, views


def int_value(kind, value):
    if kind == TYPE_INT32:
        return struct.unpack(">i", value[:4])[0]
    if kind == TYPE_INT64:
        return struct.unpack(">q", value)[0]
    if kind == TYPE_DOUBLE:
        d = struct.unpack(">d", value)[0]
        return int(d) if math.isfinite(d) else 0
    return 0


def write(xuid, gamertag, body, allowed=None, now=None):
    """A write from a player (body after the op byte). allowed: the XUIDs this player may
    write for (itself and the players of its room); others are ignored."""
    session, target, views = parse_write(body)
    now = time.monotonic() if now is None else now
    if allowed is not None and target not in allowed:
        log.info("stats: %016X wrote for %016X, not in its room: ignored", xuid, target)
        return
    tag = gamertag(target) if callable(gamertag) else gamertag
    for view, props in views:
        if is_skill_view(view):
            values = {pid: int_value(kind, v) for pid, kind, v in props}
            if RELATIVE_SCORE not in values:
                continue
            key = (session, view)
            if key not in STORE.pending and len(STORE.pending) >= MAX_PENDING:
                settle(now, force=True)
            match = STORE.pending.setdefault(key, {"writes": {}, "tags": {}})
            match["at"] = now
            match["writes"][target] = (values[RELATIVE_SCORE], values.get(SESSION_TEAM, -1))
            match["tags"][target] = tag
            log.info("stats: %016X wrote view %08X for %016X '%s': relative score %d team %d "
                     "(session %016X)", xuid, view, target, tag, values[RELATIVE_SCORE],
                     values.get(SESSION_TEAM, -1), session)
            continue
        v = STORE.view(target, view, tag, create=True)
        if v is None:
            continue
        for pid, kind, value in props:
            v["columns"][str(pid & 0xFFFF)] = [kind, value.hex()]
        log.info("stats: %016X wrote view %08X for %016X: %d properties", xuid, view, target,
                 len(props))
        STORE.save()


def settle(now=None, force=False):
    """Rates the games whose writes have settled."""
    now = time.monotonic() if now is None else now
    changed = False
    for key in list(STORE.pending):
        match = STORE.pending[key]
        if not force and now - match["at"] < SETTLE:
            continue
        del STORE.pending[key]
        session, view = key
        writes = match["writes"]
        if len(writes) < 2:
            log.info("stats: session %016X view %08X: one player, not rated", session, view)
            continue
        teams = {}
        for xuid, (score, team) in writes.items():
            teams.setdefault(team if team >= 0 else ("solo", xuid), []).append((xuid, score))
        ranked = sorted(teams.values(), key=lambda t: -max(s for _, s in t))
        places, place, last = [], -1, None
        for i, team in enumerate(ranked):
            best = max(s for _, s in team)
            if best != last:
                place, last = i, best
            players = []
            for xuid, _ in team:
                v = STORE.view(xuid, view, match["tags"].get(xuid), create=True)
                if v is not None:
                    players.append((xuid, v["mu"], v["sigma"]))
            places.append((place, players))
        for xuid, (mu, sigma) in rate(places).items():
            v = STORE.view(xuid, view)
            v.update(mu=mu, sigma=sigma, games=v["games"] + 1)
            log.info("stats: %016X view %08X: mu %.3f sigma %.3f skill %d, %d games", xuid,
                     view, mu, sigma, skill_of(mu, sigma), v["games"])
        changed = True
    if changed:
        STORE.save()


def parse_read(body):
    request, count = struct.unpack_from(">IB", body, 0)
    xuids = list(struct.unpack_from(">%dQ" % min(count, 100), body, 5))
    pos = 5 + 8 * len(xuids)
    specs = []
    for _ in range(min(body[pos], 64)):
        view, n = struct.unpack_from(">IB", body, pos + 1)
        pos += 5
        cols = list(struct.unpack_from(">%dH" % min(n, 64), body, pos + 1))
        pos += 2 * len(cols)
        specs.append((view, cols))
    return request, xuids, specs


def column(v, col, view):
    """(type, 8 value bytes) of a column of a stored view (None: no view)."""
    if is_skill_view(view):
        mu, sigma, games = (v["mu"], v["sigma"], v["games"]) if v else (MU0, SIGMA0, 0)
        if col == COL_SKILL:
            return TYPE_INT64, struct.pack(">q", skill_of(mu, sigma))
        if col == COL_GAMES:
            return TYPE_INT64, struct.pack(">q", games)
        if col == COL_MU:
            return TYPE_DOUBLE, struct.pack(">d", mu)
        if col == COL_SIGMA:
            return TYPE_DOUBLE, struct.pack(">d", sigma)
        return TYPE_NULL, bytes(8)
    stored = (v or {}).get("columns", {}).get(str(col))
    if not stored:
        return TYPE_NULL, bytes(8)
    return stored[0], bytes.fromhex(stored[1])


def ranking(view):
    """XUID hex -> (rank, rating) of everyone with the view, and how many they are."""
    STORE.load()
    rated = []
    for key, p in STORE.players.items():
        v = p["views"].get(str(view))
        if v is None:
            continue
        if is_skill_view(view):
            rating = skill_of(v["mu"], v["sigma"])
            rated.append((-(v["mu"] - 3 * v["sigma"]), key, rating))
        else:
            rated.append((0, key, 0))
    rated.sort()
    return {key: (i + 1, rating) for i, (_, key, rating) in enumerate(rated)}, len(rated)


def read(xuid, body, gamertag_of=None):
    """The STATS_ROWS answer to a read (body after the op byte)."""
    settle()
    request, xuids, specs = parse_read(body)
    out = [struct.pack(">IB", request, len(specs))]
    size = 5
    for view, cols in specs:
        ranks, total = ranking(view)
        rows = []
        for x in xuids:
            key = "%016X" % x
            p = STORE.players.get(key)
            v = p["views"].get(str(view)) if p else None
            tag = (p or {}).get("gamertag") or (gamertag_of(x) if gamertag_of else "") or ""
            rank, rating = ranks.get(key, (0, 0))
            if v is None and is_skill_view(view):
                rating = skill_of(MU0, SIGMA0)
            tagb = tag.encode()[:15]
            row = struct.pack(">QIqB", x, rank, rating, len(tagb)) + tagb + bytes([len(cols)])
            for col in cols:
                kind, value = column(v, col, view)
                row += struct.pack(">HB", col, kind) + value
            rows.append(row)
        head = struct.pack(">IIB", view, total, 0)
        size += len(head)
        kept = []
        for row in rows:
            if size + len(row) > MAX_ROWS_BYTES:
                break
            kept.append(row)
            size += len(row)
        out.append(struct.pack(">IIB", view, total, len(kept)) + b"".join(kept))
    log.info("stats: %016X read %d view(s) of %d player(s): %s", xuid, len(specs), len(xuids),
             ", ".join("%08X" % v for v, _ in specs))
    return b"".join(out)
