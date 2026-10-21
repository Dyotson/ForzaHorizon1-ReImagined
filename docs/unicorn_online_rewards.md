# Event-earned unicorns in multiplayer: investigation

Status: source trace against main `3e975fd`, followed by one-client native TU4
tests on a loopback-only server, 2026-10-20. The local experiment and the subsequent
organizer-controlled server reward service passed the native cases below.
See [server_events.md](server_events.md) for operation, persistence and protocol
limits. Remote-player enforcement remains unproven.

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
from the save. At that baseline there was no server grant check: locally added unicorns
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

The local experiment below exercises denial at this boundary. Next trace and
validate the corresponding remote-player path. Returning false locally must not
be presented as server-authoritative enforcement.

Private receipts are under `build/unicorn-online-trace-20261020/` in the workspace:
`trace-results.json`, game/server logs and labeled bounded state snapshots. They
are not committed; neither are raw decoded game code, screenshots, saves or keys.

## Opt-in local eligibility experiment

`src/hooks/unicorn_eligibility.cpp` (under `forza-recomp/`) wraps TU4's native
`0x829457A0` check. With the environment variable absent, the local experiment
is disabled; a participating server's reward policy is applied independently.
The disc build contains no experimental hook because its
corresponding function/layout has not been validated.

* `FORZA_UNICORN_ELIGIBILITY_TEST=deny`: reject the seven unicorn IDs for the
  local player while networking and the online game mode are active. The native
  caller handles the failed eligibility result; the hook does not delete cars,
  change the selected car or directly write lobby status bytes.
* `FORZA_UNICORN_ELIGIBILITY_TEST=grant`: simulate local permission, preserving native
  class/event restrictions. It cannot override a server's denial and is not an organizer-issued grant.
* Missing or unrecognized values disable the experiment. Remote players and AI are
  left to native behavior, as is single player (online mode zero / networking off).

A denial now changes the game's own dialog to explain the event unlock and direct
the player to Select Car. It retains the native OK action. Decision logging is
coalesced; no separate HUD overlay is shown. This remains a local experiment,
explicitly labeled in the dialog, rather than the full chooser proposed above.

The standalone `tests/unicorn_eligibility_test.cpp` covers all seven models,
ordinary/unknown models, disabled/granted/denied modes, offline/online contexts,
local versus other players, and preservation of a native rejection. Compile with
assertions enabled and run it as the other standalone C++ tests in this project.
Native behavior must additionally be validated; policy tests alone cannot prove
that a denied car selection returns control or that another car can launch.

### Native experiment results

Two disposable profiles were copied independently from the validated unicorn
fixture and run on a loopback-only server. The candidate was linked privately
against the existing instrumented TU4 objects and SDK; the normal installed game
was not replaced. Both TU4 and the disc no-op translation unit compiled, the
standalone policy assertions passed, and the private TU4 link succeeded.

| Test | Observed result |
| --- | --- |
| Single player with `deny`, while connected to the local server | Saleen S5S Raptor loaded normally in free roam and appeared by name in the pause menu. |
| Confirm the same Saleen in a private custom lobby with `deny` | Native eligibility was true; the hook returned false. Status changed `1 -> 2`, the experimental HUD explained the event unlock, and the game showed its own Change Your Car dialog. |
| Recover from denial with owned Lancer Evo X (1009) | Native eligibility and final result were true. Status returned `1 -> 0`; the Lancer subsequently loaded into the race with status 4. No lobby deadlock occurred in this one-client test. |
| Fresh run with `grant`, same Saleen (1197) | Native eligibility and final result were true. Status returned `1 -> 0`, and the Saleen loaded into the custom race with model 1197/status 4. |
| Garage preservation after the denial/recovery run | Authenticated database audit still found exactly one of each of the seven unicorns. Source fixture save hashes were unchanged. |

The native rejection flow **allocates a rental** and explicitly informs the
player; it does not hold the original selection behind the proposed custom dialog.
That is an observed behavior of reusing the existing eligibility failure path,
not the finished UI design. The prototype does not remove the owned unicorn or
rewrite the garage to delete it. A production chooser needs to handle this native
fallback deliberately rather than promise that a false eligibility result alone
implements the mockup.

Both game sessions left their races/party through the native Quit confirmation,
then their windows were closed normally. The private test command's host-quit
callback was unavailable in this candidate; normal window close was used instead.
No real multiplayer service, second player, long race, anti-spoofing defense or
server-issued reward was tested. Native denial was exercised with the Saleen; the
other six model IDs were covered by the standalone policy matrix, not seven
separate native race attempts.

Receipts and the native denial screenshot are kept outside Git under
`build/unicorn-eligibility-test-20261020/`. The environment variable is intentionally
not exposed as a player-facing setting, and the feature remains disabled by default.

### Native dialog and free-roam follow-up

TU4's lobby popup builder is `0x827931E8`: its second argument indexes a
32-byte descriptor table. Popup 19 is the restriction/rental notice; popup 24 is
No Cars Available. The builder opens screen `0xF1` through `0x82675B00`, passing
a parameter reference with title at `+0xC` and body at `+0x10`. Native text
references use the same allocation, UTF-16 construction, assignment and release
sequence already used by the native graphics menu.

The experiment scopes replacement to those two popup constructions following a
new local policy denial (five-second, consumed ticket, online context required).
Other dialog IDs and global localization entries are untouched. It preserves
all caller registers around the extra native text calls and retains the game's
existing callbacks. No custom Choose Car/Cancel callback has been implemented.

Two additional independent disposable profiles exercised the following:

* Custom lobby: Saleen 1197 was denied, the native rental notice displayed the
  event-unlock explanation, and OK returned control. Unrelated Game Setup and
  Quit confirmations retained their original text.
* Horizon World entry, mode 7/preset 7: confirming Saleen 1197 returned false
  despite native eligibility being true. Native status became 2, the player
  stayed out of the world, and the corrected native dialog directed them to
  Select Car. Unlike the custom race, this path did **not** allocate a rental;
  its old No Cars Available/purchase-content text was misleading.
* Selecting owned Lancer 1009 after that denial returned true and status 0;
  the normal countdown resumed and the Lancer loaded into free roam with status
  4. The earlier profile also tested denial when returning from an active world
  to its lobby and selecting another car for the next free-roam session.

The mode-7 skip in `0x82929818` is not a demonstrated bypass: the picker return
still calls `0x829457A0` directly and blocks the denied selection. No speculative
patch to that skip was added. Policy assertions now include custom modes 8/9
as well as 5/7/10. TU4 compilation, disc no-op compilation and private link passed.
Both follow-up games were closed normally after leaving their online sessions.
Authenticated garage audits still found each unicorn exactly once, and both
source fixture inventories were unchanged.

These remain one-client, localhost tests. They do not validate a second player's
join-in-progress, in-world garage changes, remote enforcement, authenticated
event rewards, or host migration. The restriction remains off by default. Private
receipts and native screenshots are under `build/unicorn-eligibility-test-20261020/`,
outside Git and separate from #191.

### Organizer service native validation

The subsequent server implementation is documented in `server_events.md`. A
fresh disposable profile used the actual `--event-unlocks` server with the local
deny/grant environment variable **unset**:

* The owned Saleen 1197 loaded into single-player free roam while connected to
  the restricted server. Its name was confirmed in the native pause menu.
* An organizer explicitly enrolled that test identity. Before an award, selecting
  the Saleen in Horizon World was denied with the native Event Unlock Required
  dialog, status 2, outside the world.
* A named event for the observed session recorded attendance through client polls.
  The organizer approved the Saleen with `owned_models=[1197]`. The database
  recorded one permanent grant and delivery state `owned`, without a car gift.
* Selecting the same Saleen again was accepted; it loaded into online free roam
  with model 1197/status 4. The Message Center displayed both the community event
  announcement and the reward-unlocked News message.
* Revoking that event grant blocked the next Saleen selection after returning to
  the lobby. Owned Lancer 1009 recovered normally and loaded with status 4.
* After normal shutdown, the authenticated garage audit still found every unicorn
  exactly once and the source fixture inventory was unchanged.

Server tests cover durable grants, delivery retries after partial persistence,
notification deduplication after clearing/restart, enrollment mismatches, the
organizer HTTP boundary and old-client capability handling. Standalone native
cache assertions cover stale/reordered replies, expiration, registration changes
and revocation. Both TU4/disc client sources compile and the private TU4 link
passes. A final WELCOME extension installs the policy before sign-in completes;
its wire contents and cache handling are also covered by the protocol tests.

Private receipts and screenshots are in `build/server-events-test-20261020/` and
the fixture `build/unicorn-eligibility-test-20261020/server-event-online/`, outside
Git. These are cooperative one-client tests, not proof of remote admission or
resistance to modified clients. Ownership confirmation for gift suppression is
explicitly manual in this first organizer workflow.

### Two-client free-roam validation

Two independent disposable TU4 clients used one loopback `--event-unlocks`
server, separate UDP ports, saves/caches, online XUIDs and credentials. Both were
explicitly enrolled and owned all seven unicorns; only EventEarned received the
organizer's Saleen 1197 grant. The local deny/grant test override was unset.

* Before its award, the host's Saleen was denied. Actual session attendance and
  the organizer award allowed that same car to load. Delivery stayed `owned`,
  with no gift or duplicate car. The unlock survived a client restart.
* EventUnearned joined the running world in an ordinary native rental (Scion on
  the first run, Ford GT on the repeat). Its lobby displayed EventEarned's Saleen;
  the server reported both humans in the same world and relayed their traffic.
* After End Free Roam returned both clients to the lobby, EventUnearned selected
  its owned Saleen. The native check denied it, displayed Event Unlock Required,
  and retained local model 1197/status 2. The host continued to launch and drive
  its Saleen with status 4; the rejected choice did not freeze the host.
* This exposed a recovery defect: the running-world lobby replaces Select Car
  with Jump In, whose native preflight looped No Cars Available for status 2.
  The fix retains the exact local policy-rejected game/player/model. Only the
  two launch callbacks, in mode 7 with a running world and that same rejected
  local status, reopen the existing World picker and stop that launch attempt.
  The automatic lobby-construction preflight is deliberately not redirected.
* On the rebuilt client, Jump In reopened Garage Select. Selecting owned Lancer
  1009 was accepted, and a subsequent Jump In loaded it with status 4 into the
  host's still-running world. Native memory and the rendered Lancer confirmed
  recovery. Existing native menu construction/return paths can still show the
  old No Cars Available notice for an invalid choice; the launch action now
  provides the recovery picker.
* The rewarded host switched Saleen 1197 -> Lancer 1009 -> Saleen 1197 through
  End Free Roam and Select Car, loading each with status 4 while the unearned
  player remained in the shared session. The server database still contained
  exactly one active grant for EventEarned and none for EventUnearned.

The explicit `FORZA_LIVE_IDENTITY_FILE` development override is necessary here:
different `user_data_root` save directories alone still use the installation's
shared online identity. Its normal default path remains unchanged.

Both clients left multiplayer normally and were closed before authenticated
garage audits. Each retained all seven unicorns exactly once; source fixture
inventories were unchanged. TU4/disc compilation, policy/cache assertions and
the private TU4 link passed. Receipts, process logs and memory snapshots are in
`build/two-client-events-20261020/`, outside Git.

These tests cover ordinary join in progress and lobby selection/recovery with
two cooperating local clients. They do not establish
an unearned unicorn already selected before an initial join, remote validation
against modified clients, all seven model assets, invites or host migration.

### In-world workshop follow-up (2026-10-21)

The same two disposable profiles exposed a separate bypass in Dak's Garage:
EventUnearned could select and drive its owned Saleen while in the shared world.
The workshop accepts a database row through TU4 `sub_827C73F0`, without calling
the lobby's per-player eligibility function. Its row's native `CarId` field is
the model ordinal used by the server reward cache.

The workshop callback now checks that permission before the native callback
changes player choices, saves, or closes the picker. This guard requires an
active multiplayer game; the single-player callback continues unchanged. A
rejected choice opens the native OK dialog above the retained picker, with
workshop-specific event-unlock text and the same pending/unavailable/enrollment
messages as the lobby. It does not assign a rental or change lobby status.

On the rebuilt client, with the local test override unset:

* EventUnearned's owned Saleen 1197 was denied with Event Unlock Required. Its
  original Fiat 1115 and status 4 remained intact. OK returned to the picker.
* From that picker, owned Lancer 1009 was accepted and loaded into free roam
  with status 4. The rendered Lancer confirmed the ordinary-car recovery.
* EventEarned changed Saleen -> Lancer -> Saleen through the workshop. The
  final Saleen was accepted with server permission Allowed, retained status 4,
  and drove in the shared world. Both clients initially occupied the same
  garage exit; moving the host clear confirmed the rendered Saleen separately.
* The server still reported two humans in one world, exactly one active Saleen
  grant for EventEarned, and zero grants for EventUnearned.

TU4/disc compilation, policy/cache assertions and the private TU4 link passed.
Both clients exited multiplayer normally and were closed before the final
authenticated save audit. Both garages retained all seven unicorns exactly
once, and the source-save inventories were unchanged. Private receipts are in
`build/garage-unicorn-test-20261021/` and `build/two-client-events-20261020/`.
This adds an owned-car workshop acceptance test, not authoritative enforcement
against modified clients or validation of borrowed/clan cars and every game
mode. Raw assets, screenshots, memory and disposable saves remain outside Git.

### Additional multiplayer mode tests (2026-10-21)

Fresh copies of the stopped two-client fixtures used a separate loopback server
data directory and room. EventEarned still had only its existing Saleen grant;
EventUnearned had no grants. The local eligibility override remained unset.
The following tests used public **custom** lobbies, not the matchmade Races or
Playground Games playlists:

| Game type | Unearned Saleen | Earned client / unearned client loaded models | Extent |
| --- | --- | --- | --- |
| Circuit | Event Unlock Required; ordinary selection recovered | Saleen 1197 / owned Lancer 1009 | Two-client race start, then retirement |
| Point-to-point | Event Unlock Required; ordinary selection recovered | Saleen 1197 / owned Lancer 1009 | Two-client race start, then retirement |
| Street | Event Unlock Required; ordinary selection recovered | Saleen 1197 / owned Lancer 1009 | Two-client race start, then retirement |
| Infected | Event Unlock Required; ordinary rental fallback | Saleen 1197 / rental Gallardo 1322 | Short timed round completed, XP results returned |
| King | Event Unlock Required; ordinary rental fallback | Saleen 1197 / rental Gallardo 1322 | Short timed round completed, XP results returned |
| Cat & Mouse | Event Unlock Required; ordinary rental fallback | Saleen 1197 / rental Gallardo 1322 | Opposing mouse teams after Auto Balance; race start, then exit |

Native memory confirmed local status 4 and `in_game = 1` for both clients in
each recorded start. Infected and King were configured to class S for the
Saleen test. Cat & Mouse used class S for both roles; this did not test switching
roles during a race. Default game-type restrictions first rejected the rewarded
Saleen as natively ineligible: the grant did not override class restrictions.
An initial Infected round used ordinary rentals on both clients; the rewarded
Saleen was then explicitly selected and tested in the subsequent short round.

The Rental Garage list and Rabbit preview/color UI were opened, but explicit
rental confirmation was not completed; only automatic rental recovery and its
subsequent race loading are established above. Club borrowing is not yet
validated: the server fixture has a local club with both members, but the native
picker still reports no membership until the festival Car Club initialization
flow is entered. Rally, private custom lobbies and the two matchmade playlists
also remain outside this test matrix.

Before the input-harness restart, both clients exited normally and the stopped
save audit authenticated both garages, retained all seven unicorns exactly once,
and verified the source fixture inventories were unchanged. Private state
receipts and logs are in `build/two-client-modes-20261021/` outside Git. A separate
private runtime expands only the test command's ordinary controller-input
whitelist for map/club navigation; it does not change entitlement decisions.

## Remaining native trace and acceptance criteria

Use disposable host/joiner profiles, one ordinary model and one unicorn, with
`FORZA_LOBBY_LOG=1`, `FORZA_EVENT_TRACE=1` and `FORZA_WORLD_TRACE=1`. Existing logs
provide lobby/status timing. Extend the established local model-selection mapping
to remote-player serialization and receive/validation, including borrowed and
rental cars. Keep raw game memory and derived code outside Git.

Exercise host and joiner entry, public/custom sessions, invites (including restart
to accept), free-roam join in progress, lobby and in-world car switches, race
transitions, host migration, reconnect and server policy changes. Verify checks
also apply outside world playlist 3 and do not affect single-player races or AI.

Before enabling restrictions, demonstrate earned/denied/pending/unavailable and
revoked cases; forged XUID, old client and stale response handling; transfer/borrow
bypass attempts; restart persistence; duplicate award retries; delivery failure;
and simultaneous invite/chat/restriction UI. A denied choice must return control
to that player without freezing everyone else's lobby or changing their save.
