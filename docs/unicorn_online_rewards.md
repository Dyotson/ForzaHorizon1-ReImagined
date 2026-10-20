# Event-earned unicorns in multiplayer: investigation

Status: source trace against main `3e975fd`, plus a one-client native TU4 trace on
a loopback-only server, 2026-10-20. No multiplayer restriction or reward service
is implemented by this investigation. Remote-player validation remains unproven.

## Agreed scope

Single player may freely edit its garage. On a participating multiplayer server,
using any of the seven unicorn models requires an event unlock issued by that
server. The requirement covers all multiplayer, including free roam, custom games,
invites and car changes after joining. Local ownership and server permission are
separate. The launcher save-editor work remains in PR #191 and is not a dependency
of the online permission system.

The seven model IDs are 320, 326, 330, 1082, 1197, 1233 and 1488. Do not use
`Data_Car.IsUnicorn` as the restriction list: it includes barn-find cars too.

## Source trace

| Stage | Existing code | What it establishes / next trace |
| --- | --- | --- |
| Player registration | `server/forza_live_server.py`, `ForzaLive.on_hello` | HELLO carries client-supplied token, XUID, features and friends key. Registration itself does not prove ownership of the XUID. |
| Existing identity binding | `server/forza_friends.py`, `Store.claim`, `Friends.hello` | First-use key binding protects friends operations (`friends_ok`). It is not currently a general login or permission check for gameplay/title services. |
| Title-service identity | `server/forza_live_lsp.py`, `service_call` | Dispatch reads XUID from `x-userid`; adding an unlock lookup here alone would trust a client-supplied identity. |
| Discovery and invites | `server/forza_live_server.py`, `on_match`; `src/kernel/live_xmsg.cpp`, `XSessionCreate`, `XSessionMembers`, `XInviteGetAcceptedInfo` | Covers matchmaking, session membership and invite acceptance. `XSessionMembers` maintains slots; it does not validate a selected car. Hiding search results would not cover direct invites or switching cars. |
| Lobby update | `src/hooks/world_events.cpp`, `sub_8287F670`; `src/debug/lobby_log.cpp`, `LobbyLogTick` | Already runs each frame and can trace mode/state/host/loading transitions. The existing event behavior filters to world playlist 3; new eligibility logic must not inherit that filter. |
| Launch readiness | `src/debug/lobby_log.cpp`, `sub_82876D90`; `src/hooks/world_drop_in.cpp`, `sub_82875FB8` | Existing host launch/bucket checks are candidate observation points. A global false return could strand an entire lobby; first establish the individual player's car-validity path. |
| Round load and status | `src/hooks/world_events.cpp`, `sub_82875840`, `sub_8286A468`, `sub_82880A88` | Round start leads to `sub_8286A2E0`. Status 1 is car select, 3 loading, 4 in game. These are observation points, not yet proven safe places to reject a selection. |
| Join in progress | `src/hooks/world_drop_in.cpp`, `JoinInProgressSite` | Existing disc return sites `0x82893B20` (host handles readiness) and `0x8288DCD8` (peers add a player's car) identify another admission path. They are not function entry addresses. |
| Running car | `src/hooks/world_challenge.cpp`, `ReadCars` | Can distinguish local human vehicles from AI/remote vehicles. It reads position/orientation, not a validated `Data_Car.Id`; do not guess a model offset from this structure. |
| Organizer gifts | `server/forza_live_lsp.py`, `is_admin`, `gift_request`; `server/forza_market.py`, `gift_stock_car` | An admin can already deliver a stock car via Message Center. No durable event entitlement is currently recorded. |
| Transfers | `server/forza_market.py`, `create`, `send_car_gift`, `retrieve_car_gift`; `server/forza_clubs.py` | Auctions, player gifts and club borrowing require policy checks; a transferred garage row must not itself become evidence of an event unlock. |
| In-game feedback | `src/platform/hud_overlay.h`, `HudShow`; `src/kernel/live_invites.cpp` | Existing HUD supports readable prompts and keyboard/controller handling. `HudShow` replaces one shared message: sustained restriction prompts need arbitration with invites, friends/chat and challenges. |

Paths beginning `src/` above are under `forza-recomp/`. Guest addresses are disc
names; use `guest_addr.h` / generated TU4 mappings and the correct layout for each
supported executable. Do not reuse disc structure offsets blindly for TU4.

## Permission and reward design

Persist server-owned grants by authenticated player identity and model, with event
ID, grant ID, organizer and timestamp. Grant/revoke actions require the existing
admin boundary. A repeated request for the same event/player/model must be
idempotent. Never expose the admin token to a player client.

Record the permission independently of garage delivery. An attendee who already
has a locally inserted unicorn must still get its online unlock. A failed or
uncollected gift must not erase an earned grant. Keep delivery in a durable queue
with retry/deduplication rather than assuming the grant and current market JSON
save are one transaction. Revocation removes online permission, not local cars.

Define authenticated ownership and recovery before awarding real event unlocks.
The existing friends key is a useful starting point but is sent in the current
protocol, and first-use XUID binding alone does not establish a trusted account.
Bind entitlement requests and admission decisions to an authenticated session,
not an arbitrary XUID header. Include protocol version/capability negotiation;
an older client cannot silently bypass an enabled restriction policy.

A permission decision must cover server/session, player identity, selected model,
request/selection generation and policy revision. Reject stale replies after a
car switch, server switch, disconnect or revocation. Do network I/O asynchronously;
never block the game update thread waiting for a permission response.

An unsupported/modified client can lie about its selected car. Client UI checks
improve behavior for normal clients but do not alone provide authoritative
enforcement. Trace whether the host/server can independently validate the selected
model in admission and peer gameplay before claiming this is enforced. The existing
matchmaking records and relay do not provide that validation today.

## In-game messages

Suggested copy (not yet wired):

* Before an unearned car enters multiplayer: **Event unlock required** / "Earn
  [car] in a community event to use it on this server. You can still drive it in
  single player." Actions: **Choose another car**, **Cancel**.
* While a permission check is pending: "Checking your event unlocks..." Keep the
  player out of the transition until a current decision is available.
* If verification is unavailable: "Couldn't verify this car's event unlock.
  Retry, choose another car, or return to single player." Do not call it unearned.
* After a grant is synchronized: **Event reward unlocked** / "[car] is now
  available in multiplayer on [server]." Show whether a garage gift is pending
  separately; an already-owned model needs no duplicate car to gain permission.

Use a deliberate car-selection gate with controller/keyboard actions, not a
brief toast for a blocked transition. Never silently substitute or delete a car.
Use the existing Message Center for reward delivery/history. Coalesce repeated
checks so messages do not appear every frame, and avoid exposing credentials in logs.

## First native trace: confirmed observations

Used a new disposable copy of the previously validated ten-car garage, the existing
instrumented TU4 executable (rexglue `0.10.0.24-dev.gbd833a2`, SHA-256
`0d29408ae38d152ef13c3f2233ed380f06acba18084c660a082b95403dfca472`), and this
branch's localhost server. Only the owned process was inspected, through bounded
read-only memory reads. No memory patches or new native hooks were used.

Entered Find Game / Horizon World, loaded the solo online free roam, returned to
the party, created a private custom circuit lobby, selected the locally inserted
Saleen and Lotus, and loaded the Lotus into the native custom race. Quit normally,
then closed the host. The source fixture's save-file inventory was unchanged.

The custom lobby is mode 5; Find Game was mode 7. Both reached status 3 (loading)
and 4 (in game). This directly confirms why a world-playlist-only hook is insufficient.
Initial online selections were Ford GT and VW Rabbit, independently of the save's
single-player active car.

For the observed TU4 local-human player implementation, vtable `0x8202682C`:

* Virtual method `+0x70`, `0x82601F08`, returns the pointer at `player + 0xD0`.
  Byte `+6` of that record is the observed player status.
* Virtual method `+0x78`, `0x825E7218`, lazily allocates/returns the car descriptor
  at `player + 0xD4`. Big-endian u32 at descriptor `+8` matched `Data_Car.Id` in
  every observation below. These offsets are not established for other builds or
  remote-player implementations.

| Observed screen/state | Live model ID | Player status |
| --- | --- | --- |
| Ford GT, world lobby | 348 | 0 |
| VW Rabbit GTI, custom lobby | 1107 | 0 |
| Saleen S5S, committed custom-lobby choice | 1197 | 0 |
| Lotus highlighted in car picker, before confirming | **1197 (Saleen remains committed)** | 1 |
| Lotus confirmed, back in lobby | 330 | 0 |
| Lotus loaded into custom race | 330 | 4 |

The distinction between highlight and commit is now observed, rather than inferred
from the save. There is currently no server grant check: locally added unicorns
can be chosen online and the Lotus loaded into a race.

Native TU4 paths inspected in the local executable image:

* `0x827B2048` opens the custom-lobby car selector. Its call to status setter
  `0x8291AEA0` returns at `0x827B2074`, matching the live log's transition to 1.
* Picker return routine `0x82792F88` reads the picker result and populates the
  player's car descriptor. The garage-result path calls `0x825E7720` at
  `0x82793084`; the alternate descriptor path calls `0x825E6738` at `0x827930F0`.
  These are source-traced paths, not independently instrumented call counts.
* The return routine calls the existing per-player eligibility function
  `0x829457A0(status_record, player)` at `0x8279312C`. Its false branch sets status
  2; the normal valid branch can set status 0 at `0x827931A0`. Both observed
  confirmed selections logged status `1 -> 0` with return address `0x827931A4`.
* `0x829457A0` obtains that player's descriptor through vfunc `+0x78`, evaluates
  bucket/game restrictions, and returns a boolean. It is a candidate to combine
  native eligibility with a cached event entitlement. It is not yet a validated
  enforcement hook: we have not forced a denial or tested all its callers.
* Round start `0x829269E8` and status setter `0x8291AEA0` were observed in both
  modes. Do not make network requests synchronously from either path.

The best next experiment is an opt-in **local** denial at the native eligibility
boundary, preserving the original result for ordinary cars, single player and AI.
Prove that the native status-2 path permits choosing another car without freezing
the lobby, then trace and validate the corresponding remote-player path. Returning
false locally must not be presented as server-authoritative enforcement.

Private receipts are under `build/unicorn-online-trace-20261020/` in the workspace:
`trace-results.json`, game/server logs and labeled bounded state snapshots. They
are not committed; neither are raw decoded game code, screenshots, saves or keys.

## Remaining native trace and acceptance criteria

Use disposable host/joiner profiles, one ordinary model and one unicorn, with
`FORZA_LOBBY_LOG=1`, `FORZA_EVENT_TRACE=1` and `FORZA_WORLD_TRACE=1`. Existing logs
provide lobby/status timing; they do not yet prove the model-selection mapping.
Trace the native car-list filter, selection commit, per-player model serialization
and receive/validation path before adding a new hook. Correlate model IDs with the
garage database and distinguish model ID from garage row ID, including borrowed
and rental cars. Keep raw game memory and derived code outside Git.

Exercise host and joiner entry, public/custom sessions, invites (including restart
to accept), free-roam join in progress, lobby and in-world car switches, race
transitions, host migration, reconnect and server policy changes. Verify checks
also apply outside world playlist 3 and do not affect single-player races or AI.

Before enabling restrictions, demonstrate earned/denied/pending/unavailable and
revoked cases; forged XUID, old client and stale response handling; transfer/borrow
bypass attempts; restart persistence; duplicate award retries; delivery failure;
and simultaneous invite/chat/restriction UI. A denied choice must return control
to that player without freezing everyone else's lobby or changing their save.
