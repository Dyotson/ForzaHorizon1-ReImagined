"""Forza Live: an optional "players online" message in a Discord channel.

Set FORZA_DISCORD_STATUS_WEBHOOK to a channel webhook URL. The server posts one message
there and edits it whenever the player count or the number of open Horizon Worlds changes
(and every 10 minutes, so its "updated" time stays fresh); on shutdown it marks the server
offline. Webhooks may edit their own messages, so no bot is needed. The message id is kept
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


def summary(status):
    """(players, worlds) from the server's status() dict, without probe registrations."""
    players = sum(len(peers) for room, peers in status.get("rooms", {}).items()
                  if room not in HIDDEN_ROOMS)
    worlds = sum(1 for w in status.get("worlds", {}).values() if w.get("humans", 0) > 0)
    return players, worlds


def render(players, worlds, online=True, now=None):
    now = int(now if now is not None else time.time())
    if not online:
        return (f"🔴 **Forza Horizon 1: ReImagined — public server offline**\n"
                f"Back soon. Address: `{PUBLIC_ADDRESS}` · since <t:{now}:R>")
    who = "1 player online" if players == 1 else f"{players} players online"
    open_worlds = "1 Horizon World open" if worlds == 1 else f"{worlds} Horizon Worlds open"
    return (f"🟢 **Forza Horizon 1: ReImagined — public server**\n"
            f"**{who}** · {open_worlds}\n"
            f"Address: `{PUBLIC_ADDRESS}` (set by default in the launcher) · updated <t:{now}:R>")


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


def due(players, worlds, now=None):
    """Whether the message needs an edit now (count changed, or the heartbeat is due)."""
    now = now if now is not None else time.time()
    return (players, worlds) != _state["key"] or now - _state["sent"] >= HEARTBEAT


def update(status, data_dir, now=None):
    """Called from the server loop; edits only when due."""
    if not enabled():
        return
    players, worlds = summary(status)
    if not due(players, worlds, now):
        return
    if post(data_dir, render(players, worlds, now=now)):
        _state["key"], _state["sent"] = (players, worlds), (now if now is not None else time.time())


def offline(data_dir):
    if enabled():
        post(data_dir, render(0, 0, online=False))
