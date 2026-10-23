# Hosting a Forza Live server

`server/` is a standard-library Python server: UDP rendezvous/relay plus the HTTP title
services (Turn 10's services, kept-alive connections) and a JSON status page.

| Port | Protocol | Use |
|------|----------|-----|
| 21200 | UDP | game, rendezvous, relay |
| 21201 | TCP | title services, status page at `/` |

Forward both on the router to the host. Players set `live_server = "your.host:21200"` (or
`FORZA_SERVER=your.host:21200`). The title services are reached at the same IP as the UDP
address, so LAN players can use the LAN IP and everyone else the public name; players
forward nothing (UDP hole punching, server relay as fallback).

## Docker

    docker build -t forza-live server/
    docker run -d --name forza-live --restart unless-stopped --network host \
        -e FORZA_ADMIN_TOKEN=choose-a-token \
        -v /path/to/data:/data forza-live --host 0.0.0.0

Use `--network host`: with a bridge network the players' public ip:port is hidden behind
Docker's UDP proxy. The optional world events need the game's `gamedb.slt`
(`--game-db`, not included; mount it into the container).

Unraid: copy `server/` to `/mnt/user/appdata/forza-live/src`, build there, run with
`-v /mnt/user/appdata/forza-live/data:/data`. `docker inspect forza-live` shows the token.

## Behind a tunnel (playit.gg)

When the ISP drops unsolicited UDP from the internet (TCP still works), put a tunnel agent
in front. Run the agent on the same machine with `--net=host` and create two tunnels to the
host's LAN IP: UDP 21200 and TCP 21201. Every player then reaches the server from the agent's
local address, each on its own source port. Start the server with that address as a trusted
proxy:

    docker run -d --name forza-live --restart unless-stopped --network host \
        -e FORZA_ADMIN_TOKEN=choose-a-token -v /path/to/data:/data \
        forza-live --host 0.0.0.0 --http-port 21201 --proxy-http-port 56678 \
        --trusted-proxy 192.168.100.105 --trusted-proxy 127.0.0.1

`--proxy-http-port` is the TCP port the tunnel publishes (56678 for UDP 56677): the server
tells it to players that arrive through a trusted proxy as their title servers' port (the
game takes that port from the welcome, so without it a tunnelled game connects to 21201 on
the tunnel's address, finds nothing and says "The Horizon server is currently unavailable":
no playlists, no leaderboards, no XP). Players connecting directly still get `--http-port`.
(`--trusted-proxy` is repeatable and takes IPs or CIDRs; `FORZA_TRUSTED_PROXIES` takes a
comma separated list.) Add 127.0.0.1 only if the agent forwards to 127.0.0.1; use the host's
LAN IP as the tunnel target otherwise. Players then set `live_server` to the tunnel's public
address. For a trusted proxy:

- the per-address limits (16 players, 32 HTTP connections) do not apply; the datagram,
  registration and error rates are counted per tunnelled player (per source port) instead.
  The server-wide player cap and every player's relay budget stay;
- its players are told no public address, so other players do not punch to the agent's
  socket and their traffic goes through the server relay. Players connecting directly keep
  hole punching. The log says `via proxy` on joins and the status page has `via_proxy`;
- its address no longer counts as "this machine" for the operator endpoints (a tunnelled
  request from 127.0.0.1 would otherwise be admin): use `FORZA_ADMIN_TOKEN`.

Without the option nothing changes. Only list addresses you control: a trusted address gets
no per-address protection.

## Operator endpoints

`GET /gift?to=<gamertag|XUID>&credits=<n>[&message=..]` or `&car=<id>`,
`GET /notify?to=..&type=..`, and `GET /world/event?session=all|<id>[&event=<id>]` are
operator-only. They are answered for requests from the server's own machine (loopback, not
through a reverse proxy), or with `FORZA_ADMIN_TOKEN` (header `X-Admin-Token: <token>` or
`?token=<token>`); everything else gets 403. From the host or Unraid terminal:

    docker exec forza-live python3 -c "import urllib.request as u; print(u.urlopen('http://127.0.0.1:21201/gift?to=Name&credits=50000').read())"

or from your LAN: `curl -H "X-Admin-Token: <token>" "http://192.168.1.x:21201/gift?..."`.
Without the token variable, remote use is impossible. Do not expose the token on the
internet side (it travels in plain HTTP).

## Restarting the server (#93)

A restart does not disturb the players. The server writes who is registered (player id,
token, XUID, gamertag, room, addresses, presence, the sessions each player hosts or is in,
the id counter and the epoch) to `live_state.json` in the data folder: after a change (at
most every 2 s), every 30 s while players are on, and on shutdown (Ctrl+C, SIGTERM,
`docker stop`). A server that starts and finds a file at most 10 minutes old loads it. The
games send HELLO with the same token every 5 s from the same socket, so they find their
registration, keep their player id and the epoch, and their lobby or Horizon World goes on
(the relay, the standby host and the world's host are all restored). Players that do not
come back are dropped after 30 s as usual. No game update is needed: v0.1.12 and later
behave the same. Keep the data folder across restarts (`-v ...:/data`); without it, or after
a crash that left no recent file, the old behaviour applies (the games sign in again under new
ids and a lobby breaks up). Not kept: a head-to-head challenge in progress, a world event
that was requested but not taken.

## Limits (all untrusted input)

- per address (not for `--trusted-proxy` addresses): 16 players, 2500 datagrams/s, 2 new registrations/s, 32 HTTP connections
  (they stay open while idle: the game needs that);
- per player: relay budget 256 KB/s (`--rate-limit`), 30 open auctions, 50 unretrieved
  gifts, 100 shared cars, 4 game invites per 30 s (the rest are dropped and logged);
  one upload at most 2 MB, HTTP bodies at most 8 MB;
- whole server: ghosts, cars and market blobs at most `FORZA_DISK_CAP_MB` (default 4096) MB,
  at most `FORZA_MAX_PLAYERS` (20000) player records;
- the status page lists gamertags and XUIDs but no IP addresses.

- shader lists (below): 1 MiB and 500 pipelines per upload, 5000 pipelines per player and UTC day,
  8 storage versions and 200000 pipelines per version.

Players are not authenticated (the XUID is whatever the client claims), except for the friends
list and the shader lists, which need the friends key bound to the XUID.

### Names are plain ASCII

A name of combining marks, foreign scripts or emoji would be drawn on every other player's
screen, so only ASCII reaches the game. `forza_limits.clean_gamertag` keeps letters, digits
and single spaces (at most 15, like an Xbox 360 gamertag) after transliterating accents
(`José` becomes `Jose`); when nothing is left the name is `Driver` plus 4 hex digits of
the XUID. It runs on the HELLO name, the sign-in (Logon2) name and every name read back
from the data files, and again when a name is written into a reply (`forza_xtsw.py`), so
stored names can never reach the game unclean. Club tags (4), names (20), descriptions (128)
and photo titles and descriptions are printable ASCII (0x20-0x7E; an empty club name
becomes `Club <id>`). Text chat drops control characters, bidi overrides, zero-width
characters and combining marks and is at most 200 characters. The launcher's Gamertag
setting and the game apply the same rule. Names the game puts inside its own peer-to-peer
session data are not seen by the server.

Game invites (#90) are delivered only between games that let their player pick whom to
invite and ask the invited player before joining (v0.1.13 and later; they say so in their
HELLO). Games up to v0.1.12 invite every player online with one press of Invite Friends and
join every invite they get at once (a player in free roam has their game restarted into the
party): their invites are dropped, and none are delivered to them. One player's invites are
capped at 4 per 30 s. `--no-invites` (env `FORZA_INVITES=0`) drops every invite; a server
older than this check needs it while v0.1.12 games are around.

Friends (#92) are kept per player (XUID) in `friends.json` in the data folder: each
player's gamertag, when it was last seen, a SHA-256 of its friends key, and its friends,
requests both ways and blocks (written at most every 2 s after a change and on shutdown;
keep the data folder). The first game that uses an XUID's friends binds them to its friends
key (`friends_key` in the player's `live_identity.txt`); another game claiming that XUID
gets no list. A player with no friends, requests or blocks is forgotten after 180 days.
Limits: 200 friends, 50 requests waiting each way, 200 blocks per player; 10 requests a
minute (bursts of 5), 60 other changes a minute, 30 searches a minute; searches answer at
most 20 players. A request to a player who blocked the sender looks sent but is dropped,
and so are invites from a blocked player. The status page shows how many players,
friendships and open requests there are. Games up to v0.1.14 still see everyone online as a
friend.

Text chat (#92) is relayed, not stored: a message goes to the players online in the
sender's session, Horizon World, or to one friend, and the server keeps only the last 200
messages in memory (time, sender, channel, recipients, text) for moderation; they are gone
when the server stops. Limits: 200 characters per message, 5 messages per 10 s per player
(the sender is told to slow down), direct messages only between friends; a player's mutes
(kept in `friends.json`) and blocks hide the other's messages. Games up to v0.1.14 have no
chat. Moderation (operator endpoints, same rules as `/gift`):

- `/admin/chat`: the last messages, and the mutes and bans with their seconds left;
- `/admin/mute?who=<gamertag or XUID>&minutes=N`: the player's messages are refused (they
  are told for how long); `minutes=0` lifts it;
- `/admin/kick?who=...&minutes=N`: drops the player and refuses its sign-in for N minutes
  (a ban; the game shows "server error 4: removed by the server admin"); `minutes=0` lifts
  it.

Mutes and bans are kept in `moderation.json` in the data folder until they run out. Example:
`curl -H "X-Admin-Token: <token>" "http://host:21201/admin/mute?who=Name&minutes=30"`.

The relay limit is 256 KB/s per player (bursts of 4 s): a game sends each peer one datagram a
frame, about 100 bytes per car it runs, so a player in a full Horizon World (7 peers) needs about
106 KB/s through the relay at 144 fps and 181 KB/s at 240 (measured with four games, #12;
`docs/online_vision.md`, "Three and four players"). Players with a direct path to each other do
not use the relay.

## Leaderboards on the website (#148)

`forza_leaderboards.py` serves the scoreboards as public, read-only JSON on the HTTP port (the
same one as the status page; 56678 on the public tunnel):

- `GET /leaderboards`: the boards that have scores (`id`, `name`, `unit`, `better`, `players`,
  the class ids on it).
- `GET /leaderboards/<board>?class=<id|letter>&limit=<1-100>`: the top rows (default 50). Board
  ids are `speed-trap-230`, `speed-zone-101`, `event-199` (events and Rivals) and
  `time-trial-7` (monthly Rivals). A row has `rank`, `player` (an opaque id), `gamertag` (the
  ASCII name the game shows), `score` (km/h for speeds, seconds for times), `unit`, `display`,
  `carId`, `car`, `classId`, `class` (D to R1) and `date`.
- `GET /leaderboards/snapshot`: every board with the top 50 of each class, in one document.

No XUIDs are in the output. The player id is a hash with a secret salt kept in the data folder
(`leaderboards_salt.txt`; keep it so ids stay the same across restarts). Answers are cached for
60 s and the server builds at most 120 answers a minute (429 beyond). Replies carry
`Access-Control-Allow-Origin: *`, like the rest of the HTTP port.

The server ships no board or car names: without them boards show as "Speed trap 230" and cars
as "Car 1398". To name them, write `leaderboard_names.json` from your own copy of the game and
put it in the data folder (read again when it changes):

```
python3 tools/extract_map_points.py --game extracted/game --update extracted/tu4 \
    --out /tmp/map_points.json --names forza_live_data/leaderboard_names.json
```

The file is `{"boards": {"speed-trap-230": "Rt. 29 Speed Trap", "event-199": "..."},
"cars": {"1398": "2012 Lamborghini Aventador LP700-4"}}` (the in-game names of the speed traps,
speed zones and events, and the cars). The website has the same board names in
`site/data/map_points.json`, so the page shows them either way; only the boards without a place
on the map (monthly Rivals, events off the Colorado map) and the cars need the server's file.

The website (https) cannot fetch a plain-http tunnel (mixed content), so the `pages` workflow
(`.github/workflows/pages.yml`) copies `/leaderboards/snapshot` from
`http://forza-reimagined.playit.plus:56678` into the site as `data/leaderboards.json` every 30
minutes, and `site/leaderboards.html` reads that file. A scheduled run that cannot reach the
server fails and leaves the published site as it was. Nothing else is needed at deploy but the
tunnel publishing the HTTP port. To try the page against a local server:
`leaderboards.html?api=http://127.0.0.1:21201`.

### The map (#316, #324)

The page's map of Colorado is our own drawing, not the game's map image:

- `site/images/colorado-map.svg`: every road of Colorado (the interstates, main and secondary
  roads, dirt roads, the towns and the festival), drawn by `tools/build_map_roads.py` from the
  road layout in a copy of the game: the sat-nav road graph
  (`media/tracks/colorado/colorado.nav`). Its lanes (a freeway's carriageways, one-way pairs)
  are joined into one centreline per road and simplified; roads are drawn by class (freeway,
  main, secondary, dirt). The terrain regions, the reservoir and the landmark icons are drawn
  from `tools/map_terrain.json`. Its colours are the page's CSS tokens (`--map-*`), for the
  light and dark themes.
- `site/data/map_points.json`: the place and in-game name of every speed trap, speed zone,
  event (Rivals) and landmark, written by `tools/extract_map_points.py` from a copy of the game
  (`--game extracted/game --update extracted/tu4 --out site/data/map_points.json`). Only
  coordinates and names.

To redraw the map from your own copy of the game:

```
python3 tools/build_map_roads.py --game extracted/game \
    --points site/data/map_points.json --out site/images/colorado-map.svg
```

`--ghosts COPY_OF_DATA_DIR/ghosts` (the server's Rivals ghosts, copied) and `--carlog drive.log`
(`tools/carlog.py`) add players' driven lines where the network has no road.

## Shader lists from players (#276)

The launcher's *Prebuild shaders* step builds a list of pipelines before play. Launchers upload
pipeline *descriptions* (shader hashes and decoded render state, plus how to rebuild a shader from
the player's own files; never shader code or game data) and the server publishes what several
players saw ([shader prebuild](shader_prebuild.md#phase-3-lists-from-players-276)). Both endpoints
are on `--http-port` (`forza_shader_lists.py`):

- `POST /shader-lists/upload`: JSON `{"version", "pipelines", "shaders"}` in the list format
  (`tools/pipeline_list.py check`). Needs `X-Xuid` (16 hex) and `X-Friends-Key` (32 hex, the
  `friends_key` of `live_identity.txt`): the XUID must have joined the server and be bound to that
  key (a 403 otherwise). Every field is checked against the list format and its enum ranges; a bad
  entry rejects the request (400). Over the size caps: 413, 400; over the daily cap: 429.
- `GET /shader-lists?version=fbo:20260228/20261018`: the merged list of one storage version, for
  anybody. It holds the pipelines reported by at least `MIN_PLAYERS` (3) different players (a
  player counts once per entry, whatever it re-sends; players are kept as a hash of the XUID) and
  the recipes of their shaders, plus an optional seed list.

Data: `shader_lists.json` in the data folder (atomic writes, at most a few MB; keep it with the
rest of the data folder). To seed the published list, put a list for one storage version in
`shader_lists_seed.json` beside it (the launcher's `data/pipelines-4D5309C9.json` fits; entries
are checked on load). Nothing else is needed at deploy: the module ships with the other
`server/*.py` files (the Dockerfile copies them), and a tunnel must publish the HTTP port as for
the other title services (the launcher reaches it at the server's UDP port plus one, 56678 for
the public server).

## Players online in Discord (optional)

Set `FORZA_DISCORD_STATUS_WEBHOOK` to a channel webhook URL (Discord: channel settings >
Integrations > Webhooks). The server posts one message there and edits it when the number
of players or open Horizon Worlds changes (and every 10 minutes); on shutdown it says the
server is offline. The message id is kept in the data folder (`discord_status.json`), so a
restart edits the same message. `FORZA_PUBLIC_ADDRESS` sets the address it shows. With
Docker, keep the URL in an env file: `--env-file /path/discord.env`.
