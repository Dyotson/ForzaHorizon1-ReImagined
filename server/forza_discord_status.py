"""Forza Live: an optional "players online" message in a Discord channel.

Set FORZA_DISCORD_STATUS_WEBHOOK to a channel webhook URL. The server posts one message
there and edits it whenever the player count, the number of open Horizon Worlds or the
online lobbies change (each lobby's playlist or custom game type and its players, so
players can see where everyone is), and every 10 minutes, so its "updated" time stays
fresh; on shutdown it marks the server offline. Webhooks may edit their own messages, so no bot is needed. The message id is kept
in DATA_DIR/discord_status.json, so a restart edits the same message instead of posting a
new one. Probe registrations (room "probe") are not counted.
"""
import json
import logging
import os
import time
import urllib.error
import urllib.request

log = logging.getLogger("forza-live")

WEBHOOK = os.environ.get("FORZA_DISCORD_STATUS_WEBHOOK", "").strip()
PUBLIC_ADDRESS = os.environ.get("FORZA_PUBLIC_ADDRESS", "forza-reimagined.playit.plus:56677")
HEARTBEAT = 600  # seconds between edits when nothing changed
HIDDEN_ROOMS = {"probe"}

_state = {"key": None, "sent": 0.0}


def enabled():
    return WEBHOOK.startswith("https://discord.com/api/webhooks/")


# Find Game's playlists (server/forza_live_lsp.py HOPPERS) and the game types a public
# custom game is found by (docs/title_services.md, Find Custom Game). A private custom game
# publishes 0xFFFFFFFF, which nobody can search for.
FIND_GAME = {3: "Horizon World", 1: "Races", 2: "Playground Games"}
CUSTOM_GAME = {2: "Circuit Race", 3: "Point-to-Point", 4: "Street Race", 5: "Infected",
               6: "Cat and Mouse", 7: "King", 8: "Free Roam", 9: "Rally"}
PRIVATE = 0xFFFFFFFF
MAX_LOBBY_LINES = 12


def lobby_name(playlist, game_type):
    if playlist == PRIVATE:
        return "Private custom games"
    if game_type == playlist and playlist in CUSTOM_GAME:
        return f"Custom {CUSTOM_GAME[playlist]}"
    return FIND_GAME.get(playlist) or CUSTOM_GAME.get(playlist) or f"Playlist {playlist}"


def lobbies(status):
    """{lobby name: [(players, slots), ...]}: each published session once, its players
    the most of its host's filled slots and the players who published it (host or member;
    a session whose host left is kept by its members' standby records)."""
    sessions = {}
    for room, peers in status.get("rooms", {}).items():
        if room in HIDDEN_ROOMS:
            continue
        for peer in peers:
            for kind in ("match", "standby"):
                m = peer.get(kind)
                if not m:
                    continue
                key = (room, m["session"])
                s = sessions.setdefault(key, {"members": set(), "filled": 0, "slots": 0,
                                              "name": None, "hosted": False})
                s["members"].add(peer.get("id"))
                if kind == "match" or not s["hosted"]:
                    s["name"] = lobby_name(m["playlist"], m.get("game_type"))
                    s["slots"] = m.get("slots", 0)
                    if kind == "match":
                        s["hosted"], s["filled"] = True, m.get("players", 0)
    out = {}
    for s in sessions.values():
        players = max(s["filled"], len(s["members"]))
        out.setdefault(s["name"], []).append((players, max(s["slots"], players)))
    for name in out:
        out[name].sort(reverse=True)
    return out


def summary(status):
    """(players, worlds, lobbies) from the server's status() dict, without probe
    registrations; lobbies as lobbies() gives them. The worlds are its Horizon World
    sessions: status()["worlds"] also counts a custom lobby whose members last searched
    the Horizon World playlist."""
    players = sum(len(peers) for room, peers in status.get("rooms", {}).items()
                  if room not in HIDDEN_ROOMS)
    found = lobbies(status)
    return players, len(found.get(FIND_GAME[3], [])), found


def render_lobbies(found):
    """One line per kind of lobby, the busiest first."""
    order = sorted(found.items(), key=lambda kv: (kv[0] == "Private custom games",
                                                  -sum(p for p, _ in kv[1]), kv[0]))
    lines = []
    for name, sessions in order[:MAX_LOBBY_LINES]:
        total = sum(p for p, _ in sessions)
        who = "1 player" if total == 1 else f"{total} players"
        if name == "Private custom games":
            lines.append(f"• {name}: {who}")
        else:
            rooms = " · ".join(f"{p}/{n}" for p, n in sessions[:6])
            more = f" (+{len(sessions) - 6} more)" if len(sessions) > 6 else ""
            lines.append(f"• **{name}**: {who} — {rooms}{more}")
    return lines


def render(players, worlds, online=True, now=None, found=None):
    now = int(now if now is not None else time.time())
    if not online:
        return (f"🔴 **Forza Horizon 1: ReImagined — public server offline**\n"
                f"Back soon. Address: `{PUBLIC_ADDRESS}` · since <t:{now}:R>")
    who = "1 player online" if players == 1 else f"{players} players online"
    open_worlds = "1 Horizon World open" if worlds == 1 else f"{worlds} Horizon Worlds open"
    lines = [f"🟢 **Forza Horizon 1: ReImagined — public server**", f"**{who}** · {open_worlds}"]
    if found:
        in_lobbies = sum(p for sessions in found.values() for p, _ in sessions)
        lines.append("**Online lobbies** (players/slots, join through Find Game or "
                     "Find Custom Game):")
        lines += render_lobbies(found)
        rest = max(players - in_lobbies, 0)
        if rest:
            lines.append(f"• Single player or menus: {rest}")
    elif players:
        lines.append("No online lobbies open right now: start one from Find Game!")
    lines.append(f"Address: `{PUBLIC_ADDRESS}` (set by default in the launcher) · "
                 f"updated <t:{now}:R>")
    return "\n".join(lines)


def _id_path(data_dir):
    return os.path.join(data_dir, "discord_status.json")


def _request(method, url, content):
    body = json.dumps({"content": content, "allowed_mentions": {"parse": []}}).encode()
    req = urllib.request.Request(url, body, method=method,
                                 headers={"Content-Type": "application/json",
                                          "User-Agent": "forza-live (status)"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read() or b"{}")


def post(data_dir, content):
    """Edit the kept message, or post a new one (and keep its id). Never raises."""
    try:
        msg_id = None
        try:
            with open(_id_path(data_dir)) as f:
                msg_id = json.load(f).get("id")
        except (OSError, ValueError):
            pass
        if msg_id:
            try:
                _request("PATCH", f"{WEBHOOK}/messages/{msg_id}", content)
                return True
            except urllib.error.HTTPError as e:
                if e.code != 404:  # deleted message: post a new one below
                    raise
        msg = _request("POST", WEBHOOK + "?wait=true", content)
        with open(_id_path(data_dir), "w") as f:
            json.dump({"id": msg.get("id")}, f)
        return True
    except Exception as e:  # network trouble must never affect the game server
        log.warning("discord status: %s", e)
        return False


def due(key, now=None):
    """Whether the message needs an edit now (what it shows changed, or the heartbeat is
    due)."""
    now = now if now is not None else time.time()
    return key != _state["key"] or now - _state["sent"] >= HEARTBEAT


def update(status, data_dir, now=None):
    """Called from the server loop; edits only when due."""
    if not enabled():
        return
    players, worlds, found = summary(status)
    key = (players, worlds, tuple(sorted((k, tuple(v)) for k, v in found.items())))
    if not due(key, now):
        return
    if post(data_dir, render(players, worlds, now=now, found=found)):
        _state["key"], _state["sent"] = key, (now if now is not None else time.time())


def offline(data_dir):
    if enabled():
        post(data_dir, render(0, 0, online=False))
