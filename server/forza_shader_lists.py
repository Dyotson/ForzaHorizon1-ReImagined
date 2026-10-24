"""Forza Live: the players' shader pipeline lists (#276, docs/shader_prebuild.md phase 3).

The launcher's "Prebuild shaders" builds every pipeline of a list before playing. The list the
launcher ships grows from play: after a session the launcher uploads the pipeline *descriptions*
of the player's own pipeline stores (shader hashes plus decoded render state, how to rebuild a
shader from the player's own game files), and this module publishes the entries that at least
MIN_PLAYERS different players reported. Nothing from the game is uploaded or kept: no shader
code, and every field is checked against the list format (tools/pipeline_list.py check_list), so
microcode cannot hide in an upload.

    POST /shader-lists/upload     JSON {"version": "fbo:20260228/20261018", "pipelines": [...],
                                  "shaders": {hash: recipe}}; headers X-Xuid (16 hex) and
                                  X-Friends-Key (32 hex): the player's XUID and the friends key
                                  of its live_identity.txt (forza_friends.py), which the XUID
                                  has been bound to by a HELLO. Answers {"accepted": n}.
    GET  /shader-lists?version=V  the merged list of that storage version (list format 2): the
                                  entries MIN_PLAYERS players reported, plus the seed list
                                  DATA_DIR/shader_lists_seed.json when the operator provides one.

Caps: MAX_BODY bytes and MAX_ENTRIES pipelines per request, MAX_PER_DAY pipelines per player and
UTC day, MAX_VERSIONS storage versions and MAX_PIPELINES pipelines per version kept. Players are
counted by a hash of the XUID, a player once per entry however often it reports it.
Everything is in DATA_DIR/shader_lists.json (atomic writes).
"""

import hashlib
import hmac
import json
import logging
import os
import re
import threading
import time
import urllib.parse

import forza_friends as friends

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"
MIN_PLAYERS = 3           # distinct players that must report an entry before it is published
MAX_BODY = 1 << 20        # bytes of one upload
MAX_ENTRIES = 500         # pipelines in one upload
MAX_SHADERS = 1000        # shader recipes in one upload
MAX_PER_DAY = 5000        # pipelines one player may send per UTC day
MAX_VERSIONS = 8          # storage versions kept
MAX_PIPELINES = 200000    # pipelines kept per storage version
MAX_RECIPES = 4           # different recipes kept per shader hash
MAX_SHADERS_KEPT = 400000 # shader hashes kept per storage version
MAX_SHADERS_PER_DAY = 2 * MAX_PER_DAY  # shader recipes one player may send per UTC day
TITLE = "4D5309C9"
LIST_FORMAT = 2

# (name, valid values) of a pipeline's `fields`, in tools/pipeline_list.py FIELDS order: the render
# pass key, the state, and four render targets of seven fields. test_forza_shader_lists compares
# this with the tool's table.
_RT = (15, 15, 5, 15, 15, 5, 16)
FIELD_VALUES = ((3, 32, 2, 16, 16, 16, 16, 2) +
                (4, 8, 2, 3, 2, 3, 2, 2, 2, 2, 2, 8, 2, 8, 8, 8, 8, 8, 8, 8, 8, 2) + _RT * 4)
PARTITIONS = ("main", "ui", "scene")

HEX16 = re.compile(r"^[0-9A-F]{16}$")
VERSION = re.compile(r"^(fbo|fsi):[0-9A-F]{8}(/[0-9A-F]{8})?$")
# The bits a vertex fetch entry ([index, three words]) and an export entry may use
# (tools/fxobj_shaders.py FETCH_LAYOUT_MASK and EXPORT_MASK; a test compares them).
FETCH_LAYOUT_MASK = (0x3FF00000, 0x7F3F7FFF, 0x7FFFFFFF)
EXPORT_MASK = 0x01003F3F
RECIPE_KEYS = {"type", "from", "base", "fetch", "export", "nop", "why"}
MAX_RECIPE_ITEMS = 4096

_lock = threading.Lock()
_state = {}     # loaded data
_cache = {}     # version -> published list (bytes), dropped on every change


def configure(data_dir):
    global DATA_DIR
    DATA_DIR = data_dir
    _state.clear()
    _cache.clear()


def data_path():
    return os.path.join(DATA_DIR, "shader_lists.json")


def seed_path():
    return os.path.join(DATA_DIR, "shader_lists_seed.json")


def load():
    if not _state:
        _state.update({"versions": {}, "daily": {}})
        try:
            with open(data_path()) as f:
                data = json.load(f)
            if isinstance(data.get("versions"), dict):
                _state["versions"] = data["versions"]
            if isinstance(data.get("daily"), dict):
                _state["daily"] = data["daily"]
        except (OSError, ValueError, AttributeError):
            pass
    return _state


def save():
    os.makedirs(DATA_DIR, exist_ok=True)
    tmp = data_path() + ".tmp"
    with open(tmp, "w") as f:
        json.dump(_state, f, separators=(",", ":"), sort_keys=True)
    os.replace(tmp, data_path())


def player_id(xuid):
    """What the lists keep of a player: a hash of the XUID, enough to count distinct players."""
    return hashlib.sha256(b"shader-lists|%016X" % xuid).hexdigest()[:16]


def authenticate(store, headers):
    """The XUID of the player that sent X-Xuid and X-Friends-Key, or None. Same proof as the
    friends list: the friends key must be the one the XUID was bound to (forza_friends.Store.claim,
    at a HELLO). An XUID that never joined the server is refused, so uploads come from players."""
    try:
        xuid = int(headers.get("x-xuid", ""), 16)
        key = bytes.fromhex(headers.get("x-friends-key", ""))
    except ValueError:
        return None
    p = store.get(xuid)
    if len(key) != friends.KEY_SIZE or not p or not p.get("key"):
        return None
    return xuid if hmac.compare_digest(p["key"], friends.key_hash(key)) else None


def _uint(v, count):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v < count


def check_pipeline(p):
    """A problem of one pipeline entry, or ''."""
    if not isinstance(p, dict) or set(p) != {"vs", "vs_mod", "ps", "ps_mod", "fields", "partitions"}:
        return "keys"
    for k in ("vs", "vs_mod", "ps", "ps_mod"):
        if not isinstance(p[k], str) or not HEX16.match(p[k]):
            return k
    f = p["fields"]
    if not isinstance(f, list) or len(f) != len(FIELD_VALUES) or \
            not all(_uint(v, n) for v, n in zip(f, FIELD_VALUES)):
        return "fields"
    parts = p["partitions"]
    if not isinstance(parts, list) or not parts or len(parts) > len(PARTITIONS) or \
            any(not isinstance(x, str) or x not in PARTITIONS for x in parts):
        return "partitions"
    return ""


def check_recipe(h, r):
    """A problem of one shader recipe, or ''. Hashes, vertex layout and export fields only."""
    if not isinstance(h, str) or not HEX16.match(h) or not isinstance(r, dict):
        return "hash"
    if set(r) - RECIPE_KEYS:
        return "keys"
    kind = r.get("from")
    if kind not in ("file", "bound", "missing"):
        return "from"
    if r.get("type") not in (None, "vs", "ps"):
        return "type"
    if "why" in r and (not isinstance(r["why"], str) or len(r["why"]) > 80):
        return "why"
    if kind == "bound":
        if not isinstance(r.get("base"), str) or not HEX16.match(r["base"]):
            return "base"
        fetch, export, nop = r.get("fetch", []), r.get("export", []), r.get("nop", [])
        if not all(isinstance(x, list) for x in (fetch, export, nop)) or \
                len(fetch) + len(export) + len(nop) > MAX_RECIPE_ITEMS:
            return "size"
        for e in fetch:
            if not isinstance(e, list) or len(e) != 4 or any(not isinstance(v, int) or isinstance(v, bool) or v < 0
                                  for v in e) or any(v & ~m for v, m in zip(e[1:], FETCH_LAYOUT_MASK)):
                return "fetch"
        for e in export:
            if not isinstance(e, list) or len(e) != 2 or any(not isinstance(v, int) or isinstance(v, bool) or v < 0
                                  for v in e) or e[1] & ~EXPORT_MASK:
                return "export"
        if not all(isinstance(k, int) and not isinstance(k, bool) and k >= 0 for k in nop):
            return "nop"
    elif set(r) & {"base", "fetch", "export", "nop"}:
        return "keys"
    return ""


def entry_key(p):
    return hashlib.sha1(json.dumps([p["vs"], p["vs_mod"], p["ps"], p["ps_mod"], p["fields"]],
                                   separators=(",", ":")).encode()).hexdigest()[:20]


def upload(xuid, body):
    """(HTTP status, JSON dict) for one authenticated upload."""
    if len(body) > MAX_BODY:
        return 413, {"error": "body too large"}
    try:
        data = json.loads(body)
    except ValueError:
        return 400, {"error": "not JSON"}
    if not isinstance(data, dict) or set(data) - {"version", "pipelines", "shaders"}:
        return 400, {"error": "unexpected keys"}
    version, pipelines, shaders = data.get("version"), data.get("pipelines"), data.get("shaders", {})
    if not isinstance(version, str) or not VERSION.match(version):
        return 400, {"error": "version"}
    if not isinstance(pipelines, list) or not isinstance(shaders, dict):
        return 400, {"error": "pipelines and shaders"}
    if len(pipelines) > MAX_ENTRIES or len(shaders) > MAX_SHADERS:
        return 400, {"error": "too many entries (at most %d pipelines per request)" % MAX_ENTRIES}
    for i, p in enumerate(pipelines):
        why = check_pipeline(p)
        if why:
            return 400, {"error": "pipeline %d: %s" % (i, why)}
    for h, r in shaders.items():
        why = check_recipe(h, r)
        if why:
            return 400, {"error": "shader %s: %s" % (h, why)}
    pid, today = player_id(xuid), time.strftime("%Y-%m-%d", time.gmtime())
    with _lock:
        st = load()
        day = st["daily"].get(pid)
        used = day[1] if day and day[0] == today else 0
        used_shaders = day[2] if day and day[0] == today and len(day) > 2 else 0
        if used + len(pipelines) > MAX_PER_DAY:
            return 429, {"error": "daily limit of %d pipelines reached" % MAX_PER_DAY}
        versions = st["versions"]
        if version not in versions and len(versions) >= MAX_VERSIONS:
            return 400, {"error": "unknown storage version"}
        v = versions.setdefault(version, {"pipelines": {}, "shaders": {}})
        added, kept = 0, []
        for p in pipelines:
            k = entry_key(p)
            e = v["pipelines"].get(k)
            if e is None:
                if len(v["pipelines"]) >= MAX_PIPELINES:
                    continue
                e = v["pipelines"][k] = {"p": {x: p[x] for x in ("vs", "vs_mod", "ps", "ps_mod", "fields")},
                                         "parts": [], "by": []}
            e["parts"] = sorted(set(e["parts"]) | set(p["partitions"]))
            if pid not in e["by"]:
                e["by"].append(pid)
                added += 1
            kept.append(p)
        # Only recipes of shaders a kept pipeline of this upload uses are stored; anything else
        # would grow the file without a pipeline to publish it with.
        used_hashes = {p["vs"] for p in kept} | {p["ps"] for p in kept}
        recipes = {h: r for h, r in shaders.items() if h in used_hashes}
        if used_shaders + len(recipes) > MAX_SHADERS_PER_DAY:
            return 429, {"error": "daily limit of %d shader recipes reached" % MAX_SHADERS_PER_DAY}
        for h, r in recipes.items():
            if h not in v["shaders"] and len(v["shaders"]) >= MAX_SHADERS_KEPT:
                continue
            seen = v["shaders"].setdefault(h, [])
            text = json.dumps(r, sort_keys=True, separators=(",", ":"))
            for entry in seen:
                if entry["r"] == text:
                    if pid not in entry["by"]:
                        entry["by"].append(pid)
                    break
            else:
                if len(seen) < MAX_RECIPES:
                    seen.append({"r": text, "by": [pid]})
        st["daily"][pid] = [today, used + len(pipelines), used_shaders + len(recipes)]
        for k in [k for k, d in st["daily"].items() if d[0] != today]:
            del st["daily"][k]
        _cache.pop(version, None)
        try:
            save()
        except OSError:
            log.exception("shader lists: could not save")
            return 500, {"error": "could not save"}
    log.info("shader lists: %s sent %d pipelines (%d new reports) for %s", pid, len(pipelines),
             added, version)
    return 200, {"accepted": len(pipelines), "new": added}


def _best_recipe(entries):
    """The recipe most players reported (a `missing` one only when it is the only kind)."""
    real = [e for e in entries if json.loads(e["r"]).get("from") != "missing"]
    pool = real or entries
    return json.loads(max(pool, key=lambda e: len(e["by"]))["r"])


def merged(version):
    """The published list of a storage version (a dict, list format 2): the pipelines MIN_PLAYERS
    players reported, plus the operator's seed list, and the recipes of the shaders they use."""
    with _lock:
        v = load()["versions"].get(version, {"pipelines": {}, "shaders": {}})
        pipelines = {}
        for k, e in v["pipelines"].items():
            if len(e["by"]) >= MIN_PLAYERS:
                pipelines[k] = dict(e["p"], partitions=list(e["parts"]))
        shaders = {}
        for p in pipelines.values():
            for h in (p["vs"], p["ps"]):
                if h in v["shaders"] and h not in shaders:
                    shaders[h] = _best_recipe(v["shaders"][h])
    seed = _seed(version)
    if seed:
        for p in seed["pipelines"]:
            k = entry_key(p)
            if k in pipelines:
                p = dict(p, partitions=sorted(set(p["partitions"]) | set(pipelines[k]["partitions"])))
            pipelines[k] = p
        for h, r in seed.get("shaders", {}).items():
            if h not in shaders or shaders[h].get("from") == "missing":
                shaders[h] = r
    used = {h for p in pipelines.values() for h in (p["vs"], p["ps"])}
    return {"format": LIST_FORMAT, "title": TITLE, "storage_versions": [version],
            "pipelines": sorted(pipelines.values(), key=entry_key),
            "shaders": {h: r for h, r in sorted(shaders.items()) if h in used}}


def _seed(version):
    """The operator's seed list for this version (checked entry by entry), or None."""
    try:
        with open(seed_path()) as f:
            data = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or data.get("storage_versions") != [version]:
        return None
    pipelines = [p for p in data.get("pipelines", []) if not check_pipeline(p)]
    shaders = {h: r for h, r in (data.get("shaders") or {}).items() if not check_recipe(h, r)}
    return {"pipelines": pipelines, "shaders": shaders}


def list_bytes(version):
    with _lock:
        cached = _cache.get(version)
    if cached is None:
        cached = json.dumps(merged(version), separators=(",", ":")).encode()
        with _lock:
            _cache[version] = cached
    return cached


def handle(server, method, path, headers, body):
    """(status, content type, body) for /shader-lists...; None for another path."""
    url = urllib.parse.urlsplit(path)
    if url.path not in ("/shader-lists", "/shader-lists/upload"):
        return None

    def reply(status, obj):
        return status, "application/json", json.dumps(obj).encode()
    if url.path == "/shader-lists" and method == "GET":
        version = urllib.parse.parse_qs(url.query).get("version", [""])[0]
        if not VERSION.match(version):
            return reply(400, {"error": "version"})
        return 200, "application/json", list_bytes(version)
    if url.path == "/shader-lists/upload" and method == "POST":
        xuid = authenticate(server.friends.store, headers)
        if xuid is None:
            return reply(403, {"error": "unknown player"})
        return reply(*upload(xuid, body))
    return reply(405, {"error": "method"})
