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

- per address (not for `--trusted-proxy` addresses): 16 players, 1500 datagrams/s, 2 new registrations/s, 32 HTTP connections
  (they stay open while idle: the game needs that);
- per player: relay budget 160 KB/s (`--rate-limit`), 30 open auctions, 50 unretrieved
  gifts, 100 shared cars, 4 game invites per 30 s (the rest are dropped and logged);
  one upload at most 2 MB, HTTP bodies at most 8 MB;
- whole server: ghosts, cars and market blobs at most `FORZA_DISK_CAP_MB` (default 4096) MB,
  at most `FORZA_MAX_PLAYERS` (20000) player records;
- the status page lists gamertags and XUIDs but no IP addresses.

Players are not authenticated (the XUID is whatever the client claims).

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

The relay limit is 160 KB/s per player (bursts of 4 s) because a 144 fps player sends about
50 KB/s to each peer.

## Players online in Discord (optional)

Set `FORZA_DISCORD_STATUS_WEBHOOK` to a channel webhook URL (Discord: channel settings >
Integrations > Webhooks). The server posts one message there and edits it when the number
of players or open Horizon Worlds changes (and every 10 minutes); on shutdown it says the
server is offline. The message id is kept in the data folder (`discord_status.json`), so a
restart edits the same message. `FORZA_PUBLIC_ADDRESS` sets the address it shows. With
Docker, keep the URL in an env file: `--env-file /path/discord.env`.
