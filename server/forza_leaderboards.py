"""Forza Live: the public, read-only leaderboards as JSON, for the website (#148).

    GET /leaderboards                 the boards that have scores (id, name, unit, players)
    GET /leaderboards/<board>         the top rows of one board; ?class=<id|letter>&limit=<1-100>
    GET /leaderboards/snapshot        every board in one document, for the site's scheduled copy

Board ids are "<kind>-<id>": speed-trap-230, speed-zone-101, event-199 (an event or Rivals
time) and time-trial-7 (monthly Rivals). A row has rank, player (an opaque id, never the
XUID), gamertag (ASCII, as the game shows it), score (km/h for speeds, seconds for times),
unit, display, car id and name, class id and letter, date. Only what the in-game
leaderboards already show.

Names: the server has none of its own. Without them a board is "Speed trap 230" and a car
"Car 797". An operator adds them in DATA_DIR/leaderboard_names.json:
{"boards": {"speed-trap-230": "Rt. 29 Speed Trap"}, "cars": {"1398": "2012 ..."}}, which
tools/extract_map_points.py --names writes from their own copy of the game (docs/hosting.md).

CORS: the server's HTTP replies carry Access-Control-Allow-Origin: * (response() in
forza_live_server.py), which covers these public, read-only answers.

Answers are cached for CACHE_SECONDS and the work done to build them is rate limited, so
the page cannot load the server.
"""

import datetime
import hashlib
import json
import logging
import os
import secrets
import threading
import time
import urllib.parse

import forza_limits as limits
import forza_scores as scores

log = logging.getLogger("forza-live")

CACHE_SECONDS = 60
CACHE_MAX = 512            # distinct (board, class, limit) answers kept
BUILDS_PER_MINUTE = 120    # answers built (cache misses) per minute, all callers together
DEFAULT_LIMIT, MAX_LIMIT = 50, 100

# CarClasses ids (docs/online_vision.md): 2 D, 3 C, 4 B, 5 A, 6 S, 7-9 R3-R1.
CLASSES = {2: "D", 3: "C", 4: "B", 5: "A", 6: "S", 7: "R3", 8: "R2", 9: "R1"}
MPS_TO_KMH = 3.6

# board kind -> (slug, title of the kind, name format, unit)
KINDS = {
    (scores.SPEED_TRAP, 0): ("speed-trap", "Speed traps", "Speed trap %d", "km/h"),
    (scores.SPEED_ZONE, 0): ("speed-zone", "Speed zones", "Speed zone %d", "km/h"),
    (scores.EVENT_TIME, 0): ("event", "Events and Rivals", "Event %d", "s"),
    (scores.EVENT_TIME, 1): ("time-trial", "Monthly Rivals", "Monthly Rivals %d", "s"),
}
BY_SLUG = {v[0]: k for k, v in KINDS.items()}

_lock = threading.Lock()
_cache = {}                # key -> (expires, bytes)
_builds = []               # times of recent builds
_salt = None
_names = (None, {})        # (mtime, names)
_clock = time.time


def configure(data_dir=None):
    """Forgets the cache and the secret; the scores come from forza_scores (its DATA_DIR)."""
    global _salt, _names
    with _lock:
        _cache.clear()
        _builds.clear()
        _salt = None
        _names = (None, {})


def salt():
    """A secret kept beside the data, so a player id cannot be worked back to an XUID."""
    global _salt
    if _salt is None:
        path = os.path.join(scores.DATA_DIR, "leaderboards_salt.txt")
        try:
            with open(path) as f:
                _salt = f.read().strip()
        except OSError:
            _salt = ""
        if len(_salt) < 32:
            _salt = secrets.token_hex(32)
            try:
                os.makedirs(scores.DATA_DIR, exist_ok=True)
                with open(path, "w") as f:
                    f.write(_salt)
            except OSError:
                pass
    return _salt


def player_id(xuid_hex):
    return hashlib.sha256(("%s|%s" % (salt(), xuid_hex)).encode()).hexdigest()[:12]


def names():
    global _names
    path = os.path.join(scores.DATA_DIR, "leaderboard_names.json")
    try:
        mtime = os.stat(path).st_mtime
        if _names[0] != mtime:
            with open(path) as f:
                data = json.load(f)
            _names = (mtime, data if isinstance(data, dict) else {})
    except (OSError, ValueError):
        _names = (None, {})
    return _names[1]


def clean_label(text, fallback):
    text = limits.ascii_fold(text) if isinstance(text, str) else ""
    text = " ".join("".join(c if c.isprintable() else " " for c in text).split())[:60]
    return text or fallback


def board_id(kind, sbid, stt):
    return "%s-%d" % (KINDS[(kind, 1 if stt else 0)][0], sbid)


def parse_board(text):
    """(kind, sbid, stt) of a board id, or None."""
    slug, _, num = text.rpartition("-")
    if slug not in BY_SLUG or not num.isdigit() or len(num) > 9:
        return None
    kind, stt = BY_SLUG[slug]
    return kind, int(num), stt


def board_name(kind, sbid, stt):
    slug, _, fmt, _ = KINDS[(kind, stt)]
    return clean_label(((names().get("boards") or {}).get("%s-%d" % (slug, sbid))), fmt % sbid)


def class_name(class_id):
    return CLASSES.get(class_id, "Class %d" % class_id)


def parse_class(text):
    """A class id from '6', 'S' or 'r2'; None when not one we know."""
    t = text.strip().upper()
    for cid, name in CLASSES.items():
        if t == name or t == str(cid):
            return cid
    return None


def display(score, unit):
    if unit == "km/h":
        return "%.1f km/h" % score
    minutes, seconds = divmod(score, 60)
    return "%d:%06.3f" % (minutes, seconds)


def rows_of(kind, sbid, stt, class_id=None, limit=DEFAULT_LIMIT):
    """(top rows, players on the board). rank is the place on the whole board, or in the
    class when one is asked for."""
    entries, _ = scores.STORE.ranked(kind, sbid, stt)
    unit = KINDS[(kind, stt)][3]
    car_names = names().get("cars") or {}
    out = []
    for place, e in enumerate(entries, 1):
        if class_id is not None and e["carClassId"] != class_id:
            continue
        score = e["score"] * MPS_TO_KMH if unit == "km/h" else e["score"]
        car = e["carId"]
        out.append({
            "rank": len(out) + 1,
            "overallRank": place,
            "player": player_id(e["xuid"]),
            "gamertag": limits.clean_gamertag(e["gamertag"]),
            "score": round(score, 3),
            "unit": unit,
            "display": display(score, unit),
            "carId": car,
            "car": clean_label(car_names.get(str(car)), "Car %d" % car),
            "classId": e["carClassId"],
            "class": class_name(e["carClassId"]),
            "date": datetime.datetime.fromtimestamp(
                e["submitted"], datetime.timezone.utc).strftime("%Y-%m-%d"),
        })
        if len(out) >= limit:
            break
    return out, len(entries)


def summary(kind, sbid, stt, players):
    slug, title, _, unit = KINDS[(kind, stt)]
    return {"id": board_id(kind, sbid, stt), "type": slug, "typeName": title,
            "name": board_name(kind, sbid, stt), "unit": unit,
            "better": "higher" if kind in scores.BIGGER_IS_BETTER else "lower",
            "players": players}


def boards():
    """Every board with scores, as (kind, sbid, stt), ordered by kind and id."""
    scores.STORE.load()
    found = []
    for key, board in scores.STORE.boards.items():
        if not board:
            continue
        kind, sbid, stt = (int(p) for p in key.split(":"))
        if (kind, stt) in KINDS:
            found.append((list(KINDS).index((kind, stt)), sbid, kind, stt, len(board)))
    found.sort()
    return [(k, s, t, n) for _, s, k, t, n in found]


def build_list():
    out = []
    for kind, sbid, stt, players in boards():
        b = summary(kind, sbid, stt, players)
        entries, _ = scores.STORE.ranked(kind, sbid, stt)
        b["classes"] = sorted({e["carClassId"] for e in entries})
        out.append(b)
    return {"boards": out, "classes": [{"id": i, "name": n} for i, n in CLASSES.items()],
            "generatedAt": int(_clock())}


def build_board(kind, sbid, stt, class_id, limit):
    rows, players = rows_of(kind, sbid, stt, class_id, limit)
    b = summary(kind, sbid, stt, players)
    b.update(rows=rows, generatedAt=int(_clock()))
    if class_id is not None:
        b["class"] = class_name(class_id)
    return b


def build_snapshot():
    """Every board with the top MAX_LIMIT/2 rows of each class on it (the page filters by
    class and shows the top 50); rows keep their place on the whole board."""
    out = []
    for kind, sbid, stt, players in boards():
        picked, per_class = [], {}
        rows, _ = rows_of(kind, sbid, stt, None, limits.MAX_PLAYERS)
        for r in rows:
            n = per_class.get(r["classId"], 0)
            if n < DEFAULT_LIMIT:
                per_class[r["classId"]] = n + 1
                picked.append(r)
        b = summary(kind, sbid, stt, players)
        b["rows"] = picked
        out.append(b)
    return {"generatedAt": int(_clock()), "cacheSeconds": CACHE_SECONDS,
            "classes": [{"id": i, "name": n} for i, n in CLASSES.items()], "boards": out}


def reply(status, obj):
    return status, "application/json", json.dumps(obj, separators=(",", ":")).encode()


def handle(method, path):
    """(status, content type, body) for /leaderboards...; None for another path."""
    url = urllib.parse.urlsplit(path)
    if url.path != "/leaderboards" and not url.path.startswith("/leaderboards/"):
        return None
    if method not in ("GET", "HEAD"):
        return reply(405, {"error": "method"})
    query = urllib.parse.parse_qs(url.query)
    what = url.path[len("/leaderboards"):].strip("/")
    class_id, limit = None, DEFAULT_LIMIT
    board = None
    if what and what != "snapshot":
        board = parse_board(what)
        if board is None:
            return reply(404, {"error": "no such board"})
    if "class" in query:
        class_id = parse_class(query["class"][0])
        if class_id is None:
            return reply(400, {"error": "class"})
    if "limit" in query:
        text = query["limit"][0]
        if not text.isdigit() or not 1 <= int(text) <= MAX_LIMIT:
            return reply(400, {"error": "limit 1-%d" % MAX_LIMIT})
        limit = int(text)
    key = "%s|%s|%d" % (what, class_id, limit) if board else what
    now = _clock()
    with _lock:
        hit = _cache.get(key)
        if hit and hit[0] > now:
            return 200, "application/json", hit[1]
        _builds[:] = [t for t in _builds if t > now - 60]
        if len(_builds) >= BUILDS_PER_MINUTE:
            return reply(429, {"error": "too many requests, try again in a minute"})
        _builds.append(now)
    if board:
        kind, sbid, stt = board
        if not scores.STORE.board(kind, sbid, stt):
            return reply(404, {"error": "no such board"})
        obj = build_board(kind, sbid, stt, class_id, limit)
    elif what == "snapshot":
        obj = build_snapshot()
    else:
        obj = build_list()
    data = json.dumps(obj, separators=(",", ":")).encode()
    with _lock:
        if len(_cache) >= CACHE_MAX:
            for k in [k for k, v in _cache.items() if v[0] <= now] or [next(iter(_cache))]:
                _cache.pop(k, None)
        _cache[key] = (now + CACHE_SECONDS, data)
    return 200, "application/json", data
