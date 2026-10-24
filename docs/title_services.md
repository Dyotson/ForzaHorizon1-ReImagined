# Turn 10's title services (Forza Horizon's "LSP")

Forza Horizon talks to Turn 10's servers for its Horizon sign-in, the Find Game playlists
("hoppers"), online XP, scoreboards, the storefront and more. The Forza Live server
answers the calls online play needs (`server/forza_live_lsp.py`, the protocol in
`server/forza_xtsw.py`). Everything below was read from the base `default.xex` (addresses
are guest addresses) and checked against the game's requests and its reaction to our
answers.

## Finding the title server

- The game enumerates title servers every 30 s (XAM enumerator app 0xFC, open message
  0x58039, `XTitleServerCreateEnumerator`; items `X_TITLE_SERVER {inaServer, flags,
  szServerInfo[200]}`; `live_lsp.cpp` answers it).
- It keeps a server whose description matches its service mask: `sub_83155238` calls
  `Matches(mask, description)` (`sub_83153D48`; `*` and `?`, case-insensitive) with the
  LspProvider config's `defaultMask` `Turn10.*` as the pattern. Our description is
  `Turn10.Horizon` (`FORZA_LSP_SERVICES` overrides it; `*` does not match the mask, which
  gives the "Horizon server is currently unavailable" path).
- It connects to the server's address on TCP port 80 (`net.cpp` sends it to the Forza
  Live server's HTTP port) and posts to `/Services.Horizon/o.xtsw` (the LspProvider
  config; `/Services.HorizonDemo/o.xtsw` for the demo, `/Services/o.xtsw` the default).

## Transport

Request (`sub_83152710`, templates at `0x822DC8D0` and `0x822DC990`):

```
POST /Services.Horizon/o.xtsw HTTP/1.1
Host: <server>
Content-Type: bin/xtsw
Content-Length: <n>
User-Agent: Xls Client
Connection: Keep-Alive
X-TitleId: 1297287625                                   (0x4D5309C9)
X-ClassName: Forza.WebServices.Hopper, Forza.WebServices
X-Method: GetList
X-UserId: 0x9a7b6b0a63bf2                               (the caller's XUID)
X-TransactionId: <GUID as hex>                          (transactional calls only)

<parameters>
```

**Who is calling.** `X-UserId` is the only thing in the game's request that names the player,
and anyone can send it. Our game adds `X-Friends-Key: <32 hex digits>` to every request on its
title server connections (`src/kernel/live_tcp.cpp`, `lsp_request_header.h`: the head of each
request is held until it is complete and the line goes in before its blank line), the friends
key of its Live identity (`live_identity.txt`), which the server bound to the XUID at the
game's first HELLO (`forza_friends.py`; the launcher's shader list upload proves itself the
same way). The server (`forza_live_lsp.verify_caller`) refuses a call that changes anything
(every method but `Get*`/`Search*`/`Download*`, and `Hopper.GetGameOptions`, which moves a
world's event along) when the key is not the XUID's, and takes a call without a key, from a
game before the key or for an XUID that never signed in with one, as unverified: accepted
and logged (once per XUID every 10 minutes), its race stats marked `verified: false`, and
never the player's word where it matters (`caller_trusted()`). Addresses can't do this job:
behind a tunnel (`docs/hosting.md`) every HTTP connection and UDP datagram arrive from the
tunnel agent's address, and a connection's TCP port says nothing about the player's UDP one.

The call itself is `sub_82AABAC0(client, class, method, params, results member list,
&results, ...)`; each service method has a small stub that passes its class and method
names (`Logon2` calls it at `0x82ABC0A8` with class string `0x820E242C`, method
`0x820E24A4` and results member list `0x83316F18`).

Reply: the HTTP parser (`sub_831515A8`, a byte-at-a-time state machine) reads the status
line, the headers (only `Content-Length` matters) and the body. The body
(`sub_82AA9868`, header read by `sub_82AA96F0`):

| Offset | Field | |
| --- | --- | --- |
| 0 | u32 status | 0 = success; 2 = failure, the call returns `0x80510000 \| code`; anything else `0x8050000E` |
| 4 | u32 code | |
| 8 | u32 message length | a UTF-8 message follows when status != 0 (the client logs it) |
| 12 | results | decoded by `sub_82A95910(results, member list, reader)` |

Bytes after the results are ignored, so a reply of the header and enough zero bytes reads
as empty results (zero, false, empty strings and lists) for any method; the server answers
methods it doesn't handle that way. Calls made with a "compressed" flag (none of those
seen) would carry an XMemCompress block instead (`sub_82AAD798`). A failed `GetGameOptions`
ends Find Game with "You have been disconnected from the session"; most other failures are
retried (`DLCMetadata.GetStringTableInfo` every 100 ms).

## Encoding

Parameters and results are records described by type descriptors in `.rdata`, 9 words
each: `{kind, name, offset, -, count offset, size, member list, element type, flags}`
(`0x7FFFFFFF` = none). A member list is `{first descriptor, count}` in `.data`, filled by
static initializers at startup (`lis/addi` of the first field, `li` count, `b 0x82A93090`;
the Logon2 ones at `0x831AE340`). Kinds and their wire form (big-endian, packed, fields in
declaration order):

| Kind | Type | Wire |
| --- | --- | --- |
| 0 | u8 | 1 byte |
| 1, 2 | u16, i16 | 2 bytes |
| 3, 4 | i32, u32 | 4 bytes |
| 5, 6 | i64, u64 | 8 bytes |
| 7 | float | 4 bytes |
| 8 | WCHAR string (fixed buffer) | u32 length + that many bytes (UTF-8; `DriverA` is 7) |
| 9 | struct | its fields |
| 10 | array (inline, or a pointer with count/capacity fields for results) | u32 count + elements |
| 11 | SYSTEMTIME | ISO 8601 text like a string: u32 length + `2026-10-11T15:25:00Z` (the reader, `sub_82A954D8`, parses `%4hd-%2hd-%2hd`, `T`, `%2hd:%2hd:%2hd`, an optional `.ms` and `Z` or `+hh:mm`; anything else, the empty string too, fails the whole reply with `0x8050005C`) |
| 12 | bool | 1 byte |
| 13 | GUID | 16 bytes |
| 15 | byte array | u32 length + bytes |
| 16, 17, 18 | XNKID (18) and two other fixed blobs | u32 length (must be 36, 16, 8), then the bytes |

Flags: 0 = a parameter passed by value, 3 = by pointer; 1 = a result, 5 = a result in a
caller buffer (an array whose capacity comes from a `max...` parameter). A method's
parameters are `Forza<Class><Method>_Params` and its results `..._Results` (some methods
have no results: the reply is the 12-byte header). The type names, field names and
layouts of all 292 service records can be dumped from a running game: read the
descriptors (they start at the record's name string's reference, `kind 9`) and member
lists from `/proc/PID/mem` (guest address X is host `0x100000000 + X`). The 111-byte
`Logon2` request decodes to exactly its length with this table, and `UpdateAll` (325 bytes)
too.

## Services

Classes: `Forza.WebServices.{User, Hopper, OnlineProfile, DLCMetadata, Scoreboard,
Storefront*, Auction, Gifting, Gift*, SharedCar, CarClub, DLC, DailyCreditBonus,
GameplayLog, Notifications, LargeFileUpload}` and `Xls.WebServices.{User, Clan, Content,
Notifications, Transaction}`. What the game calls, in order:

| When | Call | Our answer |
| --- | --- | --- |
| Sign-in (about 50 s after boot) | `User.Logon2(loginData)` | `responseData`: profile valid, a permission table allowing every feature (below), `userData` (XUID, gamertag, **currentXPTotal = the player's online XP**), timeouts |
| | `OnlineProfile.UpdateAllTables`, `UpdateAll` | success; `UpdateAll`'s `playerRaceHistory` (level, XP, **onlineXP**, online races and wins) and `playerHistory` (credits) are kept per player |
| | `DLCMetadata.GetStringTableInfo("EN")` | no table (`Id` -1, #79); with the empty answer the game also asked `Xls Content.DownloadContent(0, 0, 0x40000)` x2 and saved a nameless file to `marketplace:\` |
| Party lobby | `Hopper.GetList(maxCategoryResults 2, maxHopperResults 32)`; Title Update 4: `GetListEx` (each hopper also has `requiredOfferId`, u64) | the playlists (below) |
| Find Game, picking a playlist | `Hopper.GetSearchSettings(hopperId)`, `GetGameOptions(hopperId, roundNum, 0x8000)`, then (after the session search) `UserRegister(hopperId, sessionId)` every `nextRegisterSeconds` | search settings, the round's GameOptions XML, 60 s |
| Race start | `GameplayLog.Write`, `Hopper.GetRaceMultipliers` | empty, multiplier 1.0 |
| Race end | `Scoreboard.SubmitScoreEx` / `SubmitScore`, `User.SetPlayercardInfo`, `OnlineProfile.UpdateAllTables` / `UpdateAll` | the score kept (scoreboards below) / success (profile kept) |
| End of a Rivals race (also retiring) | `User.WriteRaceStatsEx(id, raceStats, scoreData)` | kept per player (`userXP`, credits, the race) |
| After the sign-in (single player too) | `Xls Notifications.RetrieveForUser` `notificationPollIntervalSeconds` after `Logon2`, then every `nextRetrieveSeconds` (`StartNewSession` on some paths) | the player's notifications (Message Center, below), 15 s then 60 s (with 0 the game polled 7 times a second) |
| Sometimes | `Xls User.SetFriends` | empty |
| Photo mode, SAVE / large photo ("uploading to forzamotorsport.net") | `LargeFileUpload.Create(payloadSize, overwriteOldest, userFileName)`, then `Xls Content.UploadContent(contentId, payloadOffset, payloadTotalSize, uploadData)` | `contentFileId`, then `payloadUploadedSize` = bytes held so far (`forza_photos.py`: kept per player under `photos/`, 2 MB each, 100 per player, in the disk quota; `DownloadContent` gives the owner's file back). Answered empty before, the game never saw its upload finish (#71); the chunk size and the answer's meaning (bytes held, not bytes of this chunk) are from the descriptors, not yet seen in a game |
| Photo mode, SHARE to the storefront (MY SHARED PHOTOS) | `StorefrontPhoto.Create(fileHeader, photoDesc, payloadSize, previewData)` (27 KB: header, car/track ids and a thumbnail), then `UploadContent` as above; the pages and files are read with `Storefront.GetPhotoPageList`, `GetFileListForPage`, `GetStats`, `GetFeaturedPhotos`, `GetMostDownloadedPhotos`, `GetTopRatedPhotos` | the shared photos below (#82) |

The game keeps one HTTP connection to the title server for all its calls. The server must
leave it open while idle: when it closed idle connections after 15 s, the game's next call
minutes later (Rivals from the single-player free roam) was never sent and failed as
"The Forza Horizon server is not available", until the game found its title server again
(about 12 minutes later in one test). `forza_live_server.py` now waits 6 hours.

### Permissions (features)

Before an online feature the game looks it up in the permission table that `Logon2`
(and `GetPermissionsTable`) returned: `sub_8264D570(feature)` -> `sub_82CE97B8` ->
`sub_82AAEE98` finds the entry whose `MethodId` is the feature id (Rivals is 61; others
used: 20, 21, 22, 28, 45, 66, 84, 123) and returns its `Access` as the error code (0 =
allowed). A feature missing from the table is error 4: the popup "FEATURE UNAVAILABLE.
The Forza Horizon server is not available at this time" without any request reaching the
server. The table is up to 256 entries `{MethodId, Tier, IsEnabled, Access}`; the server
sends ids 0-255, all enabled with `Access` 0.

### Playlists (hoppers)

`GetList` returns `hopperCategories[] {name, description, hopperStartIndex}` and
`hoppers[] {id, name, description, playerCount, allowVoteToSkip, allowVoteToKick,
allowVoteToRebalance, payoutModifier, hideTrueSkill, maxPartySize, icon,
minAllowedRaces, maxAllowedRaces, maxAllowedWins, shouldVoteCarClassBeforeLaunch,
randomizedOrder, intermissionTimeMs}`. A category holds the hoppers from its
`hopperStartIndex` to the next category's. The Find Game screen ("Playlists", built by
`sub_82730500`) lists the first category's hoppers that every party member may join
(`sub_82888988` -> `sub_828864D0`): with `-1` meaning no limit, races < `maxAllowedRaces`,
wins < `maxAllowedWins` and races >= `minAllowedRaces` (beginner playlists). Our hoppers:
3 "Horizon World", 1 "Races", 2 "Playground Games" and 20 "Co-op Festival", all in the one
category Online (a second category's hoppers would not be listed). Co-op Festival is the
Horizon World among friends (placement only into a world a friend leads), whose host starts
festival events with `/event` in World chat (`docs/coop_campaign.md`, "Milestone 1 progress");
it is listed only to games whose HELLO has feature 0x08. Horizon World is
the shared free-roam world (`docs/online_vision.md`): preset 7 rounds from the festival
(track 1101), `GameTypeFilterType="FreeRoam"`, endless (`EndConditionOptions Type="None"`;
the client runs them as an online free roam that players JUMP IN to, which has no timed
end anyway; `--world-round-minutes` serves `Type="Time" Time=` seconds for clients with
`world_drop_in` off), its own search settings (`minPlayersToLaunch` 1, intermission 5 s, 2
connect attempts); `GetSearchSettings` is answered per hopper. The game copies a
playlist's search settings into its online game object when the playlist is picked
(`+2016` min players to launch, `+2020`/`+2024` the intermission times).

`GetGameOptions` returns `gameOptionsXml`: the round's options in the format of the game's
own `media/gametunablesettings.zip` `DefaultRacesHopperOptions.xml` and
`DefaultPlaygroundHopperOptions.xml` (used for the "Quick Match" playlist when the server
is unavailable; they are compressed with zip method 21, XMemCompress LZX with a 128 KB
window: a block is `FF, u16 uncompressed size, u16 compressed size, data`, which
libmspack's `lzxd` with window bits 17 decodes). The root is `<GameOptions>` with
`GameTypePreset` (`GameOptionPresets.xml`: 1 circuit race, 2 point-to-point, 4
Infected, 5 Cat & Mouse, 6 King, 7 free roam, 198 street race), `TrackId`
(the game database's `Tracks`), `MaxPlayers`, `MaxAIPlayers`, `UseTraffic`,
`CollisionMode`, time of day and car class attributes, and children `Buckets/Bucket` (with
`CarRestrictions`), `StartingGridOptions StartType="OpenWorld"`, `EndConditionOptions
Type="NumLaps" NumLaps=".."` and `ScoringOptions`. The server's rounds for hopper 1 are the
game database's `HopperRaces` table (preset, track, laps: (1, 322, 3), (2, 155, 1), (1, 107,
3), (2, 145, 1)), picked by `roundNum`; `DATA_DIR/hoppers/<id>.xml` replaces them. The
Playground Games playlist (2) serves the game's own Infected (4, arena track 215), Cat &
Mouse (5, track 322, 2 laps) and King (6, track 215, 180 s) presets in turn
(`server/playground_rounds.xml`, copied from `GameOptionPresets.xml` with their buckets,
tag and scoring options; the game's own playground hopper default is a free roam);
`--playground-games 6,4` picks and orders them.

Tested with two consoles (fresh online profiles; both picked PLAYGROUND GAMES in Find
Game, A hosted, B joined, the game started 40 s later): **Infected** in the arena (A
"You're INFECTED, crash into the survivors!", B "You're a SURVIVOR...", both in rental Ford
GTs), ended after its 300 s with B 1st (survived) and A 2nd: `WriteRaceStats` for both
(hopper 2, place 1/2 and 2/2), `SubmitScore(type 4, 1100)` each (the online XP earned,
board -1) and `UpdateAll` with online XP 1100; then the lobby "PLAYGROUND GAMES - KING"
(Addison West Steelworks, Playground Arena, off road, 03:00 mins, both in rental Nissan
GT-R R34s, INTERMISSION counting down) and **King** at night (B "You're the KING, run away
from all the other players!", A "Crash into the KING to steal their golden Gamertag!")
(`screenshots/2026-10-11_playground_0*.jpg`). King ended after its 3 minutes with B 1st (1500
XP) and A 2nd (880 XP), online XP 2600 and 1980 (wristband 2), and the lobby went on to the
next round (Infected again). With rental cars forced
(`ForceOnlyRentalCars`) the lobby shows no car class vote. **Cat & Mouse** (an earlier
run) loaded on both (A the mouse in a red Fiat 500 "race to the finish line! PROGRESS:
0/2 LAPS", B a cat) but it only ends when the mouse finishes 2 laps, so nobody driving
never ended it.

**Cat & Mouse, driven to its end (#13, 2026-10-23).** `--playground-games 5` makes every round
Cat & Mouse (the round index is the game's own `roundNum`, kept in the save, so with a list the
first round varies from run to run). Clear Springs Sprint (track 322, 2.25 km, 2 laps) has two
teams, each with exactly one Mouse (`MinPlayers`/`MaxPlayers` 1): two players are two teams of
one, both mice (the red and the blue mouse, rental Fiat 500s, "race to the finish line!"), a
third player is a cat (B class, Nissan GT-R) in one of the teams. The earlier recorded
Millennia path is another circuit; the driver now follows the track's own AI route:
`tools/track_path.py 322 path.txt` reads `Tracks.RouteId` (2) and `MediaName` from
`gamedb.slt`, extracts `colorado/Ribbon_00/route_002.owt` from `media/aiopenworld.zip`, one
closed lap of 554 points (2228 m), and `tools/autodrive.py` with `AD_PATH_LOAD` drives it lap
after lap (TU4, 30 fps: 20-26 m/s, 1-5 m off the route, `AD_XDG`/`AD_FIFO` name the game's
scratch folders). Two rounds, A driving and B parked at the start (two TU4 games, local
server): PROGRESS 0/2, 1/2, 2/2 LAPS, then the finish (best laps 94.1 and 93.6 s), RACE RESULTS by team (1 RED TEAM 04:28.510, 2
BLUE TEAM DNF), XP REWARDS (finished 1st 1,000 XP, mileage bonus 200 XP, 2 player game x1,
skills x1.2: 1,440 XP; 1,560 in the first round), "WAITING FOR OTHER PLAYERS...", saving, and
the next lobby 35 s after the finish. B's round ended with A's finish. Server: `WriteRaceStatsEx`
for both (hopper 2, places 1/2 and 2/2), `SubmitScore` type 3 (A's best lap, board -1) and type 4
(online XP: A 1,560 then 1,440, B 660 each round), `UpdateAll` with online XP A 1,560 -> 3,000,
B 660 -> 1,320 (wristband 2). Fixed on the way: the cat's upload after a round carried only its
race history, and the player record's credits read 0 (the record now keeps the fields an upload
leaves out). The next lobby then sat at "WAITING FOR PLAYERS TO JOIN..." both times (#14,
below); with that fixed it starts after its intermission.

**The lobby that never started (#14, fixed 2026-10-23).** After a round the next Playground
Games lobby sometimes sat at "WAITING FOR PLAYERS TO JOIN..." with every player in it (first
seen after King: 10 minutes; reproduced 2026-10-15 in about 2 of 11 King -> Cat & Mouse
lobbies with two players). The log tool (`src/debug/lobby_log.cpp`, `FORZA_LOBBY_LOG=1`,
`LOBBY:` lines in the game log) writes each peer's lobby state, for the host the launch check
`sub_82876D90` and its inputs each second (humans vs minPlayersToLaunch, players still in the
last round, the bucket check's result `+2148` (TU4 `+2172`) and its due flag, CPU drivers vs the
session's, every player's state bytes), the lobby's events, every player status change and each
bucket check (`sub_82895018`: result, the caller, the table of per-team bucket counts, the
players' records). In a stuck lobby the launch check is false only because of the bucket
result: humans 2 >= 2, nobody in the last round, `+2148` = 0 with the due flag already clear.

The cause: the host puts the players in teams and buckets (the roles) with `sub_8289B618`
(TU4 `sub_8294CF88`) on the table at the online game + 2052 (TU4 + 2076; per team 5 words: its
players, then each bucket's count). It clears the table, assigns teams, then each team's
buckets in random order (`sub_82897330`: the buckets a team needs, shuffled by `sub_82888380`,
`std::random_shuffle` over the CRT's `rand()`), and returns the bucket check: 0 when a team has a
bucket outside its MinPlayers/MaxPlayers. It runs as the lobby opens after a round
(`sub_82876130`, which stores the result and clears the due flag) and in the host's check
(`sub_82875FB8`) after a player's status event (a join, a car change); nothing else runs it
again. With two players in Cat & Mouse each team of one drew Mouse or Cat; the fix-up after a
failed check (`sub_8289B0D0`'s loop) moved one player only, so when the other drew Cat the
table stayed `1 1 0 0 0 | 1 0 1 0 0` (team 2 without its mouse), the result 0, and the lobby
waited until a player changed something (both lobbies after a driven Cat & Mouse round). The
first lobby of a session passed: there the players are added one at a time as they join
(`sub_8289B450`). Setting
the due flag from outside (guest memory) ran the host's check again: the second redraw gave
`1 1 0 0 0 | 1 1 0 0 0` and the intermission started at once. The fix
(`src/hooks/lobby_buckets.cpp`): in a Find Game lobby (mode 7), on the host, a result of 0 is
drawn again at once, up to 32 times, and logged once without `FORZA_LOBBY_LOG` ("LOBBY:
team/role draw outside the buckets' limits (teams ...), drawn N times: result R (teams ...)"),
so a recurrence shows its table. A custom game (mode 10) is left alone: its launch does not
wait for this check.

Tested with the fix (TU4, local server, `FORZA_LOBBY_LOG=1`): three games with
`--playground-games 6,5` and both rounds made 60 s timed ones (a scratch copy of
`playground_rounds.xml`), then two after the third left: 8 lobbies after a round (4 King ->
Cat & Mouse, 4 Cat & Mouse -> King, half of them with three players) all started 33 s after
they opened (the 30 s intermission); one, King -> Cat & Mouse with two players, drew
`1 1 0 0 0 | 1 0 1 0 0` and was drawn again once. Then two players on the full Cat & Mouse
(`--playground-games 5`), A driven along the route: finish ("FINISHED WINNING TEAM"), results
(RED TEAM 03:10.974), XP (1,440), the next lobby drew `1 1 0 0 0 | 1 0 1 0 0`, drew again
once, counted its intermission down and the next round started ("YOU ARE THE MOUSE");
the second driven round the same (finish, results, the lobby drawn again once, round three
started). Use: `FORZA_LOBBY_LOG=1`, `grep LOBBY forza.log`; the `check:` line shows `bucket
result 0` in a stuck lobby, the `bucket check` lines its table.

`GetSearchSettings` returns the session filter (`numStartingMatchmakingQueryResults`,
QoS limits, ranking factors) and `maxConnectAttemptsBeforeCreating`,
`maxConnectSecondsBeforeCreating`, `maxWaitingForClientsToConnectSeconds`,
`minPlayersToLaunch` (2), intermission times.

### Matchmaking

Find Game searches with `XSessionSearchEx` (XGI message 0xB001C, procedure 4, up to 25
results) for property `0x10000030` = the hopper id (Find Custom Game: procedure 5, the game
type tab's value, free roam 8, which public custom games set on their host), properties `0x30008105` and
`0x30008106` (doubles, 3.0 and 1.0) and contexts `X_CONTEXT_GAME_TYPE` (0x800A) = 1 and
`X_CONTEXT_GAME_MODE` (0x800B) = 0. With no result it hosts a matchmaking session itself
(`XSessionCreate`, flags 0x2D) and registers it with `Hopper.UserRegister`; a later searcher
finds it and joins. `live_xmsg.cpp` answers `XSessionSearchEx` like `XSessionSearch` from
the sessions the room's players host for that property's value (published on the Forza
Live server), those with a free slot only, the fullest first. The **session host** calls
`GetGameOptions` for the **next** round when a round starts (the answer is the round the
lobby shows after this one; a vote to skip asks again). In the Horizon World the server
answers it with a festival event when one was requested for that world
(`server/forza_world.py`, `docs/online_vision.md` phase 2).

### Find Custom Game (#61, 2026-10-15)

A public custom game is found by its game type. The host's Create Custom Game makes a
`0x2D` session; its property `0x10000030` is `0xFFFFFFFF` while the lobby is Private
(nobody searches for that, so a private lobby is never listed) and the game type tab's
value once Setup Game is saved as Public (Circuit Race 2, Point-to-Point 3, Street Race 4,
Infected 5, Cat and Mouse 6, King 7, Free Roam 8, Rally 9): properties first, then
`XSessionModify`, which republishes the record. Find Custom Game searches each tab the
cursor rests on (procedure 5, `10000030=<type> 30008105=0 30008106=0 800A:1 800B:0`, up to
25 results) and shows the host's QoS data (game type, track, car class, time limit). With
two TU4 instances on a local server this worked as it was (King lobby listed, joined).
What could hide a public lobby, fixed:

- Every publication was one UDP datagram, sent once. One lost datagram left the server
  with the Private key, and after a server restart (or a player dropped for 30 s without
  keep-alive) the lobby was gone until it changed. The client now keeps its last record
  (and withdrawal) and sends it again with every keep-alive (5 s), with its current
  XNADDR: the record names the host by the server id it had when the game made the
  session, and after a restart a joiner waited at "Attempting to join lobby" for that
  old id. Tested: server restarted under a public King lobby; 3 s later the searcher
  listed it and joined.
- A search is asked up to three times, 0.4 s apart, before it answers no sessions; when
  the server has not welcomed us it answers none at once.
- A record is about 860 bytes (608 of them QoS data), and the answer was one datagram of
  at most 1,400 bytes: only one lobby per game type was ever listed. The answer now spans
  as many datagrams as it needs (up to 25 sessions; bit 15 of the count means another
  datagram follows), and the client waits for the last. Older clients read the first.
  Tested with a second lobby: both listed.

A player whose game never hears from the server sees "No Sessions Found" too (#69: its log
has `no answer from <server> yet` and no `signed in to`): see "Without a server answer" below.

### XP and credits

The game computes the rewards itself at the end of an online race ("XP REWARDS": finished
1st 1,000 XP, mileage bonus 200 XP, x1 for a 2-player game, x1.1 skills = 1,320 XP; 2nd
800 XP), with the server's race multiplier (`GetRaceMultipliers`). Credits go to the local
save. Online XP does not: the game takes it from `Logon2`'s `userData.currentXPTotal` at
every sign-in (a restart with 0 there showed online XP 0 and the wristband level reset,
credits kept), so the server keeps it: from each `UpdateAll` (`onlineXP`, only growing)
into `DATA_DIR/players/<XUID>.json`, back in the next `Logon2`. Without the title server
the game offers only the default "Quick Match" playlist, which awards no multiplayer XP.

### Every leaderboard (inventory, #43)

Every leaderboard a single-player game reaches, the call that writes it and the call that
reads it. Forza Horizon keeps its leaderboards on Turn 10's title services; its Xbox LIVE
stats are only TrueSkill. The title's SPA (the XDBF resource in `default.xex`) has one
stats view in its XVC2 table, `0xFFFF0000` (the ranked skill view of game mode 0) with no
columns of its own, and the XGI messages the code sends are 0xB0006-8 (user context and
properties), 0xB0010-15, 0xB0018 (sessions), 0xB001C, 0xB001E, 0xB001F, 0xB0021
(XUserReadStats), 0xB0025 (XSessionWriteStats) and 0xB0071: no stats enumerators
(XUserCreateStatsEnumeratorBy*) and no XSessionFlushStats. Score types are the `type` of
`SubmitScore` / `GetRows`.

| Leaderboard (where) | Written by | Read by | Server |
| --- | --- | --- | --- |
| Speed traps (speed cameras: 21 in `gamemodes.zip` `Colorado/speed_camera.xml`, `scoreboard_id` 201-230) | passing one: `SubmitScore(type 1, board, speed in m/s)` (`CAmbientChallengeLeaderboard`, its queue `sub_82992520`, sent at `0x829911CC`); the HUD shows the speed and the save's PB | at the trap `GetRows(type 1, view 5, board, 1, 200)` (`sub_82979C60`: type 2 for a zone, else 1); the map (filter Speed Cameras, X LEADERBOARD on one): `GetRows(1, view 2, board, 1, 50)`, X changes the filter | `forza_scores.py`, best speed per player; at the trap the game places a new score by walking up from `userRow.position` through the rows (`row[position - 2]`, `sub_82979E80`), so view 5 puts a player ranked below the 200 rows sent at place 201 (rank unchanged), and `src/hooks/ambient_leaderboard.cpp` keeps the game within the rows for any server (#288) |
| Speed zones (average speed: 9 in `Colorado/average_speed.xml`, boards 101-112) | `SubmitScore(type 2, board, average speed)` | as speed traps, type 2 | the same |
| A friend's speed beaten | `NotifyRival(board, type 1/2, rival)` (`0x82990744`) | the rival's Message Center (RIVAL CHALLENGE) | `notify_rival` ("Speed Trap 30"; the game's names are in `List_SpeedCameras.str`) |
| Events (festival events, street races, showcases; board = the event's `Events.Id`) | at the finish `SubmitScoreEx(type 3, board, best lap or race time)` (`0x829495CC`, `0x829496F8`) and the ghost (`UploadFile`) | Race Central LEADERBOARDS: an event `GetRows(3, view, board, 1, 50)`; VIEW OVERALL `GetRollupRows(0, view, pivot, eventTypeId 1-3)` | best time per player; rollups over the type-3 boards |
| Rivals (an event's ticket, X RIVALS) | `SubmitScoreEx(3)`, `NotifyRival(type 3)` | `GetRivalAndPersonalBest(board)` (`0x829BFE68`), `GetRival` (`0x8285F7F8`), `GetBestScores(3, ...)` (`0x82948F1C`), `DownloadFile` (`0x82949404`) | type-3 boards and their ghosts |
| Monthly Rivals (Race Central, RIVALS) | as Rivals, `isServerTimeTrial` | `GetTimeTrials(7, 150)` | `--time-trials` |
| Online XP (Race Central) | `OnlineProfile.UpdateAll` (the profile's `onlineXP`); `SubmitScore(4, board -1)` after an online race (`0x82949094`, not kept) | `GetRows(4, view 5, board 1)` (`0x828E19B4`) | every player's profile |
| 1000 Club (pause menu, Title Update 4) | `Challenge.ChallengeCompleted(car, thumbprint)` | `GetRows(5, view 5, board 1, 1, 200)` (`0x824CE230`), its LEADERBOARDS `GetRows(5, view 2, ...)` | medals per player |
| Car club boards | - | `GetRows(..., isClubBoard)` | the caller's club's rows |
| Storefront (Design Storefront creators) | - | `GetStorefrontRows` (`0x828E1964`) | not answered (no Storefront on the server) |
| TrueSkill (Xbox LIVE stats view `0xFFFF0000`) | at the end of an online game `XSessionWriteStats` (XGI 0xB0025, `sub_83164D60`; the task `0x82D624A8` built at `0x82D63DDC`): per player `X_PROPERTY_RELATIVE_SCORE` (0x1000800A) and `X_PROPERTY_SESSION_TEAM` (0x1000800B) | `XUserReadStats` (0xB0021, `sub_82B2B238`, called at `0x82CC0BFC`) when the party session is created | `forza_stats.py`: rated per game, rows with rank and skill |
| Skill chains, popularity, MY PROFILE (speed traps and zones triggered, ...) | the save only | the save | no online board |

Two players (A and B, copies of one save, local server): A drove the Rt. 29 speed trap
(board 230) at 162.32 km/h, B at 144.02 km/h; B's map LEADERBOARDS on that trap listed "1
DriverA R3 797 LP700-4 162.32 km/h, 2 DriverB 144.02 km/h, YOU ARE IN THE TOP 100% OF 2
PLAYERS" (A's score had been kept by the server before the restart that split the store by
type, and came back through the migration). TrueSkill, two consoles in Playground Games
(Infected, `--playground-games 4`): at each round's end the host wrote both players' rows
(`XSessionWriteStats` view `0xFFFF0000`: relative score 1/0, team 1/0, the lobby's session
id, the same for every round of the lobby) and both consoles read their row
(`XUserReadStats` view `0xFFFE0000`) at once; after two rounds the server had DriverA mu
3.72 sigma 0.79 skill 12 (rank 1) and DriverB mu 2.28 (rank 2), kept across server
restarts. The game shows no skill anywhere seen (the lobby's yellow number is the online
wristband; `hideTrueSkill` false changed nothing visible).

**Fast travel onto a speed trap (#349).** A trap takes its speed when the car enters it
(`sub_828BB140`, at least `minimumSpeed` 30 mph) from the car's velocity getter (vfunc 83,
`sub_82D1CE18`), which reads the copy of the velocity the car's physics step makes
(car+0xB10). The vehicle teleport `sub_824B87A8` (fast travel, the car reset, an event's
start) zeroes the velocity in the physics state only (vfunc 84, car-0xA10), so until the
car's next physics step (a fast travel holds it behind the loading screen, about 0.7 s
here) the car still reports the speed it had when the map paused the game. Fast travelling
onto a trap recorded that speed while the car stood on the trap: TU4, the car paused at
151.1 km/h, the fast travel's destination pointed at the Rt. 6 trap (Fast Travel Anywhere
costs 5 Tokens, which the local profile has none of), the car arrived at 0 km/h and the
game sent `SubmitScore(type 1, board 228, 41.976 m/s)`. `src/hooks/teleport_velocity.cpp`
refreshes the reported velocity from the state after each teleport: the same test sent
nothing (`TELEPORT: car ... its reported speed 146.5 km/h is now 0.0 km/h` in the log),
and traps and zones driven through still record (Rt. 29 on both builds; on TU4 the Rt. 20
speed zone, `SubmitScore(type 2, board 105, 34.137 m/s)`). The public server sets no speed
limit: players drive modified and modded cars, and with the game fixed a trap only records
speeds really driven. Speeds above a trap's record (`SPEED_TRAP_LIMIT_KMH`, all 22 traps:
the best known Xbox 360 result on that trap, from the Forza Horizon speed trap charts on
cyberscore.me.uk, charts 230405-230426, compared at 0.1 km/h) or above 470 km/h on any trap
or zone are only logged with the player's public leaderboard id ("speed trap N: ... km/h is
above the trap's record (... km/h)"; "... is more than any stock car reaches (470 km/h)").
With `--trap-limits` such scores are not kept, and kept ones are removed when the server
loads `scoreboards.json`, each logged once ("scoreboards: removed <public id>'s ... km/h
from board N type T, above ... km/h"). The public server ran with the limits once, on
2026-10-23, which removed 66 exploited results above their trap's record. A road-layout model of
each trap's reachable speed (corners, straights, grades, generous grip) was tried first and
dropped: it was not validated in the game and ordinary players beat it on corner-limited
traps (175-200 km/h on Sth Rt. 38, board 210, where it said 132).

Not done: an event board (type 3) was not driven again with the new board keys
(unit-tested); the Storefront's rows (`GetStorefrontRows`) stay empty.

### Scoreboards, leaderboards and Rivals (`server/forza_scores.py`)

The method stubs (`Forza.WebServices.Scoreboard`, all calling `sub_82A92018`) and their
callers:

| Method | Stub | Called from |
| --- | --- | --- |
| `SubmitScore(type, data)` / `SubmitScoreEx(type 3, {data, numCarsPassed})` | `82AB9570` / `82AB95E0` | the scoreboard manager `0x82897xxx` (race end), `0x828DF464` |
| `UploadFile(scoreId, data)` | `82AB9650` | `0x82897918`, after a submission answered `uploadFiles` |
| `DownloadFile(id, maxSize 256000)` | `82AB90D0` | `0x82897A1C` (a rival's ghost, by `ghostFileId`) |
| `GetBestScores(scoreType, board, isServerTimeTrial, car, class, drive type)` | `82AB9140` | `0x828976FC`; seen at the start of a Rivals race |
| `GetRivalAndPersonalBest(board, isServerTimeTrial, rivalXuid)` | `82AB9228` | `sub_8290CD58` ("RivalData: Rival Net Task"): X RIVALS on an event's ticket |
| `GetRival(board, isServerTimeTrial, rivalXuid)` | `82AB91B8` | `sub_827D09F0` |
| `GetRows(type, view, board, isServerTimeTrial, isClubBoard, startAt, maxResults)` | `82AB9330` | the leaderboard screen's fetch `sub_82826E88` (50 rows from `startAt`), `0x828C7F1C` (view 5, 200 rows) |
| `GetRollupRows(type, view, pivotId, eventTypeId, isClubBoard, startAt, max)` | `82AB9298` | `sub_82826E88` |
| `GetStorefrontRows`, `GetTimeTrials(7, 150)`, `NotifyRival` | `82AB93D0`, `82AB9460`, `82AB94E0` | `sub_82826E88`; the Rivals menu `0x827AD614`; `0x82897E38`, `0x828DE9F4` |

A row is `{position, rank, user (UserData), score, submittedTime, carId, carClassId,
carPerformanceIndex, 6 assist flags, ghostFileId (-1 none), clanData, bounty}`.

The server keeps each player's best score per board in `DATA_DIR/scoreboards.json`. A
board is the score type, `gameScoreboardId` and `isServerTimeTrial`: the ids of different
types overlap (board 230 is the Rt. 29 speed trap and the event with Id 230), so they are
kept apart (before 2026-10-15 boards were keyed by id alone; an old store is split by each
entry's type when loaded). Types 1 and 2 (speeds) and 4 (XP) rank bigger first, 3 (times)
smaller first; only types 1-3 are kept (online races submit type 4 on board -1), at most
4096 boards. A new best of an event (type 3) is answered with a fresh `scoreId` and
`uploadFiles` true; the ghost the game then uploads is kept as it is in
`DATA_DIR/ghosts/<id>.bin` (the row's `ghostFileId`) and served by `DownloadFile`. Views:
0 the top from `startAt`, 1 around the caller, others (friends are everyone here) like 0;
`isClubBoard` keeps the caller's car club's members; `userRow` is the caller's row
(position 0 when they have none). Rollups rank players by the event boards they lead. The
rival is the one asked for, or (rival 0) the score just better than the caller's (without
a score of their own, the slowest other player); the bounty is 1000 (the game showed 5,000
Cr).

Seen in the game (single player, a copy of a career save, event "GoPro Millennia
Masters", board 199): X RIVALS on the event's ticket calls `GetRivalAndPersonalBest(199)`.
With no rival the game says "There was an error communicating with the Turn 10 server";
with a rival (another player's submitted score, 235 s) it shows YOUR RIVAL, the rival's
gamertag, TARGET 03:55.000 and BOUNTY 5,000 Cr; A goes to car select, the Rivals race
starts (`GetBestScores(3, 199, car, class)` first) with the target on the HUD. Retiring
calls `WriteRaceStatsEx` (finished, place 2 of 2) and shows "FINISHED - YOU LOST".

A real race (`tools/autodrive.py` drove the car along the CPU drivers' line: the 4 laps
of Millennia Masters in the Aventador, 8th of 8): at the finish the game sent
`SubmitScoreEx(type 3, board 199, score 115.13)`: the score is the **best lap** time
(1:55.13), with the car (1398, class 7, PI 0.864), assists, parts and tuning. The server
answered `uploadFiles`, and the game uploaded its ghost of that lap at once
(`UploadFile`, 17,332 bytes: a 16-byte header `000002A0 0000410C 00000298 00000ABC`, then
compressed data), kept as ghost 2; then `WriteRaceStatsEx` (place 8 of 8) and
`GetRivalAndPersonalBest(199)` for the results screen's offer "NEW RIVAL vs <rival>,
TARGET, BOUNTY 5,000Cr: Do you want to race your Rival? NO THANKS / BRING IT". The rival
of a Rivals race is the one fetched when it started (RESTART keeps it).

**The ghost round trip and a won Rivals race** (one console, the uploaded ghost given to
another player's score, 200 s): X RIVALS on the ticket showed "YOUR RIVAL ... TARGET
03:20.000, 01:24.870 (the gap to the personal best), BOUNTY 5,000 Cr"; while the Rivals
race loaded the game downloaded the ghost (`DownloadFile(3, 256000)`, 17,332 bytes) and
the race had the ghost car in the vehicle list (kind 2, the kind of another console's
car) driving the recorded lap. A Rivals race is one flying lap against the target. After
the lap (1:47.228, `autodrive.py` on the line saved from the career race): "Congratulations!
You beat Seeded Rival! Do you want to collect the bounty and finish this Rivals event or
continue lapping?", FINISH: `SubmitScoreEx(199, 107.228)` (a new best: its ghost uploaded,
16,796 bytes), `WriteRaceStatsEx` (place 1 of 2), "FINISHED - YOU WON", and CASH REWARD:
BOUNTY 1,000 Cr (the server's `bounty`; the game multiplies it, "WRISTBAND BONUS +400%",
hence the 5,000 shown), mileage bonus 1,200 Cr, difficulty and assists bonus +20%, TOTAL
7,440 Cr (the save's balance went from 635,556 to 642,996). `NotifyRival` was not sent for
this rival (the game says it notifies "your friend or club member"; the seeded rival was
neither). Screenshots (local): `screenshots/2026-10-11_rivals_0*.jpg`.

**Two consoles, player against player** (B a second copy of the career save, A's real lap
of 1:47.228 on the board with its ghost): B's X RIVALS showed "YOUR RIVAL DRIVERA, TARGET
01:47.228, BOUNTY 5,000 Cr"; loading the race B downloaded A's ghost (`DownloadFile(4)`,
16,796 bytes) and the ghost car (kind 2) drove A's lap beside B. B's lap of 1:33.294:
"Congratulations! You beat DriverA!", FINISH: `SubmitScoreEx(199, 93.294)` (new best, its
ghost uploaded), `WriteRaceStatsEx` (1 of 2), **`NotifyRival(board 199, rivalXuid =
DriverA, type 3, myScoreId = the scoreId of B's submission)`**, and the bounty (TOTAL 7,440
Cr, B's balance 630,931 -> 638,371). With a real player as the rival the game does send
NotifyRival; the server logs it (delivering it to the rival as a Message Center
"beat you in the ... Rival Challenge" notification needs the notification format, below).

After both runs the Millennia Masters board (Race Central, Gold Wristband) showed "1
DriverB R3 797 LP700-4 01:33.294" and "2 [HZN] DriverA R3 797 LP700-4 01:47.228" with
their assists, "YOU ARE IN THE TOP 100% OF 2 PLAYERS" (the club tag comes from the car
club, below). ONLINE XP is `GetRows(type 4, board 1, view 5 "Friends & Club")`: the
server ranks every player by the online XP kept from `UpdateAll` ("1 DriverB 2,600 XP, 2
[HZN] DriverA 1,980 XP"; screenshots `2026-10-11_leaderboard_0*.jpg`). The end-of-game
`SubmitScore(type 4, board -1)` is the XP of that game.

Race Central (at the festival; the map's X fast-travels there) has LEADERBOARDS: Festival
Events (VIEW OVERALL and the seven wristbands' event lists), Street Races, Showcase
Events and Online XP. VIEW OVERALL is `GetRollupRows(type 0, view, pivot -1, eventTypeId
1)`; an event is `GetRows(type 3, view, board = the event's Id)`; X changes the filter:
view 2 Friends Only, 3 Favorites, 1 Near Me, 0 Global. Seen with the seeded score: the
event's board showed "1 Seeded Rival, S 686 Gallardo LP570, 03:55.000" with ABS/TCS/gear
columns and "YOU'RE NOT ON THE LEADERBOARD"; the overall board showed the rollup score as
a time. A on a row offers SHOW GAMER CARD and ADD AS FAVORITE (`Storefront.
AddFavoriteUser`, `GetStorefrontFavorites`; empty answers). Race Central's RIVALS has
MONTHLY RIVALS and the career events' Rivals.

**Monthly Rivals** (`--time-trials`). Entering RIVALS (`ForzaUI::CTimeTrialTypeSelect`,
`sub_827AD130`) calls `GetTimeTrials(7, 150)`; the game keeps the answer in a
`Forza::CTimeTrialDataSource` (vtable `0x82072694`): the trials (count +0x28), the
categories `{category, startIndex}` (count +0x48D08, entries from +0x48D0C, at most 8) and
a selected category at +0x20, by which its count (`sub_8283EA58`) and items
(`sub_8283F208`) are filtered. The MONTHLY RIVALS screen selects **category 4** (read
from the running game). The server's trials used to be in category 0, so the screen had
none and kept its YOUR RIVAL, BOUNTY and description spinners going; in category 4 it
lists them (name, description, route type CIRCUIT RACE, surface ASPHALT, the track's
picture) and asks `GetRivalAndPersonalBest(board = the trial id, isServerTimeTrial)` for
the selected one. The rival is a score on that board (`seed.py` with `isServerTimeTrial`
gave "[HZN] SEEDED RIVAL 04:00.500, BOUNTY 5,000 Cr"; without one the spinners stay). A
on a trial: `CTimeTrialRivals::DownloadTimeTrial` (`sub_827D1E40`) calls
`Hopper.GetGameOptions(hopperId, gameOptionsIndex, 0x8000)` (the server's
`TIME_TRIAL_HOPPER` rounds), then GARAGE SELECT (my garage or the club garage), CAR
SELECT, and the time trial loads (`GetBestScores(1001, ...)`, START RIVALS, the HUD's LAP
and TARGET 04:00.500 on track 156).

**The 1000 Club** (Title Update 4, #17). Each of 218 cars has 5 or 6 challenges, 1145 in
all (`CarChallenges` / `CarChallengeTitles` in TU4's `gamedb.slt`; the challenges
themselves are `media/carchallenges/carchallenges_<carId>.xml` in the 1000 Club
package's `2700_pri_65` zip: `pass_when` / `fail_when` groups of skills, speed traps,
A-to-B times, smashables, events). A completed challenge is a medal. The service calls:

| When | Call | Our answer |
| --- | --- | --- |
| A challenge completed (the medal on the HUD) | `Challenge.ChallengeCompleted(carId u16, challengeThumbprint u32)` | success (no results); the medal is kept per player and car, a thumbprint counted once |
| 1000 CLUB opened (pause menu) | `Scoreboard.GetRows(type 5, view 5, board 1, startAt 1, max 200)` | the 1000 Club board |
| Its LEADERBOARDS (Y) | `GetRows(type 5, view 2 Friends Only, board 1, 1, 50)`; X changes the filter | the 1000 Club board |

The board (`club_entries`) ranks every player by medals (the number of challenges the
server saw completed), the first to reach a count first on a tie; a row's car is the car
of the player's latest medal. Kept in `scoreboards.json` (`club1000`: per player, the
thumbprints per car). The game sends nothing else for the club (no score submission), so
medals earned before the server kept them, or on another server, are not on the board.
(After the medal was in the save, a few more ONE EIGHTY tries in the Aventador sent
nothing; they may not all have counted as the skill.)

Seen with two players (`docs/dlc.md`, 1000 Club): "1 DriverB 1 (medal icon) 10/15/2026,
2 DriverA 1 ...", "YOU ARE IN THE TOP 100% OF 2 PLAYERS"; the score shows as the medal
count and `submittedTime` as the date; the filters (views 2, 3, 1, 0) all list the board.
Unit tests: `server/test_forza_scores.py` (`cd server && python3 -m unittest discover -p
'test_*.py'`).

The thumbprint names the challenge: the Aventador's ONE EIGHTY SKILL sent `carId 1398,
challengeThumbprint 0xAE6828D3` both times it was completed (the second time after a
restart had lost the medal: the career must be saved, `docs/dlc.md`).

### Car clubs and the club garage (`server/forza_clubs.py`)

At the festival the CAR CLUB sign (X) calls `Xls User.GetUserData`: with no club the menu
is MY CLUB (CREATE, JOIN CLUB). JOIN CLUB is `Xls Clan.GetUsersToJoin(filter 1, 8)`: the
server lists each club by its leader ("[HZN] Seeded Rival" with a leader icon); A on it:
REQUEST TO JOIN CLUB (`RequestJoiningClanByName("HZN")`; an open club takes the player at
once, a closed one keeps the request for a leader's `ApproveRequestToJoin`), SHOW GAMER
CARD, ADD AS FAVORITE. The game said "Request sent successfully" and showed the club the
next time the sign was entered (`GetUserData` with clanId, then `GetClanMembers`,
`CarClub.GetClanDetails`): "HZN Horizon Kings" with CLUB LEADERBOARD (members), CLUB
RANKINGS, CLUB GARAGE, CLUB INFO, QUIT CLUB, CLUB TOTAL 2. CLUB GARAGE (`SharedCar.
GetClubCars`) offers SHARE CAR: picking a car and YES sends `UploadCarTextures` (the
livery thumbnail, 98,793 bytes) and `SharedCar.Create` (the car's data and tuning), "Your
car has been successfully uploaded", and the garage then lists it (`GetCarDetails` for
its thumbnail): "2012 Lamborghini R3 797, OWNER DriverA, SHARED DATE, LAST USED DATE,
NUMBER OF RACES 0" (`screenshots/2026-10-11_club_0*.jpg`). Roles as the game labels
them (`sub_8282A260`): 1 leader, 2 co-leader, 3 member. CREATE asks for the tag and name
with the on-screen keyboard (not driven in these tests; the club used here was created on
the server). Requests, invitations and the other club news go through the Message Center
(below). The game's own text (`Clan.str`) settles what "open" means: a club either
RECEIVES REQUESTS, which a leader or co-leader accepts ("The membership request ... may
have been accepted/rejected by another Leader or Co-Leader"), or is INVITE ONLY (the
game then hides REQUEST TO JOIN CLUB). The server used to let a player into an open club
at once; now it keeps the request and sends CLUB REQUEST to the leaders and co-leaders.

Tested with two consoles (DriverA co-leader of HZN, DriverB in no club): A's CAR CLUB sign
(X at the festival) showed INVITE MEMBER (co-leaders and leaders), which lists the
players the server knows outside a club (`GetUsersToInvite`: DriverB); INVITE TO CLUB
sent `InviteToClan(B)`, "The invite was sent successfully". B's next poll brought CLUB
INVITE ("[HZN] DriverA sent you an invite to join the Horizon Kings club"); after the
Message Center's first-visit popup, A JOIN, "You are about to join the Horizon Kings
club. Are you sure?", A: `AcceptClanInvite(1, id)`, the message cleared, "Would you like
to set a waypoint to the Car Club now?", and the club had 3 members (A saw DriverB in the
member list); the leader and A got NEW CLUB MEMBER. A's QUIT CLUB (two confirmations,
`LeaveClan`) sent MEMBER QUIT to the leader and B (co-leader by then). Then the other
way (HZN receiving requests, B co-leader): A's FIND CLUB, the [HZN] row, REQUEST TO JOIN
CLUB (`RequestJoiningClanByName("HZN")`), "Request sent successfully"; B's Message Center:
CLUB REQUEST "DriverA has sent a request to join your Car Club. Would you like DriverA to
join?", A ACCEPT, "Are you sure you want to add DriverA to your car club?", A:
`ApproveRequestToJoin(A)`, the message cleared, NEW CLUB MEMBER to the others; A's next
poll: MEMBERSHIP ACCEPTED "Welcome to the club. Your membership to the Horizon Kings club
has been accepted." (In an invite-only club the game offers only SHOW GAMER CARD and ADD
AS FAVORITE on that row.)

### The Auction House and car gifts (`server/forza_market.py`)

The server side is complete and unit-tested (`Auction.*`: GetGlobalConfig, Create,
UploadAuctionTextures, SearchForAuction, GetAuctions, GetDetails, GetMyAuctions,
GetMyBids, PlaceBid, RetrieveCarForAuction, RetrievePaymentForAuctionSold,
RetrievePaymentsForAuctionLost, Cancel, Restart, RemoveMyBannedAuction, ReportContent;
`Gifting.SendCarGift`, `RetrieveCarGift`, `RetrieveCreditsGift`), with the records in
`DATA_DIR/auctions.json`, `gifts.json` and the car blobs in `DATA_DIR/market/`. The
model follows the game's own tables:

- Status per viewer (the labels in `sub_827DC788`, which reads the status at +0x68 of
  its auction item = the wire record + 8): 0 active, 1 expired, 2 SOLD, 3 expired
  (cancelled), 4 active and you are the top bidder, 5 WON, 6 OUTBID, 7 LOST; the buyout
  price shows for 0, 4 and 6.
- `allowedActions` bits (the action table at `0x8323E528`, 0x28-byte entries: label,
  confirm, success title and text string ids, the bit, 1 = bidder / 2 = seller action):
  0x1 Bid, 0x2 Buyout, 0x4 ResolveWon (collect the car), 0x8 ResolveFailedAuction (the car
  back), 0x10 Restart, 0x20 Cancel, 0x40 Report, 0x80 RemoveBannedItem, 0x100 ReceiveMoney
  (an outbid bid back), 0x200 ReceiveMoney (the sale); search a player's auctions, view
  their storefront and gamercard have no bit.
- Money: the game takes a bid from its own credits; PlaceBid's `amountRefunded` gives back
  the bidder's earlier bid on the same auction; an outbid player collects theirs with
  RetrievePaymentsForAuctionLost, the seller the winning bid with
  RetrievePaymentForAuctionSold, the winner the car with RetrieveCarForAuction (marked
  `wasOriginallyFromAuctionHouse`, one more owner in its history). A bid at the buyout
  price ends the auction at once.

**Not reachable: the Auction House's screens have no scenes in this game.** None of the
menus opens it (checked: the festival's Autoshow, whose "$" tile is TRADE-IN CARS, Dak's
Garage, the Paintshop, Race Central, the Car Club, the pause menu, the party lobby), and
the reason is in the UI tables:

- The screen table (filled by `sub_82695AE0`: per screen id N, the factory at +8N+4 and the
  scene name at +8N+8 of the screen manager) maps screens 7, 8 and 0xA to the classes
  `ForzaUI::CAuctionHouseMainUI` (factory `sub_8268FDA8`), `CAuctionHouseDetailsUI`
  (`sub_8268FDF0`) and `CAuctionHouseCreateUI` (`sub_8268FE40`), all three with the scene
  name `SCENE_REMOVED`; only one other screen (0xF) has it. (`CAuctionHouseUIScene`,
  `CAuctionHouseSearchCriteriaPopup` and the data sources `CAuctionHouseSearchDataSource`
  and `CAuctionHouseMyAuctionsDataSource` are in the code too.)
- `media/UI.zip` has no auction scene at all: its `Scenes/ui4` keep FM4's storefront
  scenes (`603_d_file_details`, `605_X_MYS_MY_STOREFRONT`, `606_D_POP_FILTER`,
  `610_X_MYS_Share_Files`) but nothing for the auctions, and no string in the binary names
  one.
- The menu entries that open them are FM4's: the shared item table at `0x8323E8C0`
  (index 0: screen 0x104, a wait popup gated by `sub_82674790` on the auction config) and
  the per-menu tables at `0x8323E7C0` (indexes 3 and 6: screens 8 and 0xB) and `0x8323E408`
  (index 4: screen 0xA), used through `sub_82671420` / `sub_826714F0` by menus whose
  buttons no Forza Horizon scene has.

A hook could open screen 7, but its scene would have to be built: the screen classes
bind named controls of FM4's scene (lists, text fields, the car viewer), which the
generic Forza Horizon scenes do not have. Without the scene file format (`.bgf`) and an
editor that is out of reach, so the Auction House stays server-side only. Its
notifications (outbid, sold, won, failed) are listed by the Message Center without a
title or text.

Gifting has stubs for `SendCarGift`, `RetrieveCarGift`, `RetrieveCreditsGift`,
`MarkFileGiftRetrieved` (no `GetGiftsForUser`/`SendCreditsGift` in this game). A gift
arrives as a Message Center notification (below), whose DOWNLOAD collects it. The sender
side is `ForzaUI::CMyCars` (screen 0xA1, scene `108_C_BUY_CARSELECT`): its car options
add GIFT CAR (`sub_827093B8`, for a car that is not the one being driven), which opens
the recipient list (`sub_826EA0A0`: popup screen 0xDE over the friends list; every other
player online is a friend here) and on the choice (action 7, `sub_826F0330`) runs
`Forza::CGiftingTransactionSendCarGift` (`sub_8289D9E8` -> `SendCarGift`).

### Photos and the photo storefront (`server/forza_photos.py`, #71, #82)

Photo mode > PHOTO SHARE has BROWSE PHOTOS (PG PICKS, most downloaded, top rated, search) and
MY SHARED PHOTOS (the player's own storefront, `SLOTS AVAILABLE n/m`). Uploading from there
(a saved photo, UPLOAD PHOTO: make, model, keywords, description) is **`StorefrontPhoto.Create`**
(class `Forza.WebServices.StorefrontPhoto`, method `Create`; the descriptors name the records
`ForzaStorefrontPhotoCreate_*`, which is why #71 answered `Storefront.PhotoCreate` and
the TU4 game's real call got the empty answer: contentFileId 0, `UploadContent` to a file that
does not exist, "Your photo was NOT successfully uploaded to the Forza Horizon Storefront").
Request (about 2.3 KB here, 27 KB with a real picture's thumbnail): `fileHeader` (Title =
the saved photo's name, Description, Price, Quantity, Type 5 = photo, PayloadSize, KeywordOne,
KeywordTwo), `photoDesc` (EnvironmentId, TrackId, MakeId, ModelId, CreationTime,
ForStorefrontUse), `payloadSize` (the JPEG, 15 KB at 1280x720 for a black one), `previewData`
(a 416x240 JPEG). Answer `contentFileId`; the picture follows in `Content.UploadContent`.

The storefront reads (all `Forza.WebServices.Storefront`):

| Call | What the game does with it |
| --- | --- |
| `GetPhotoPageList(ownerXuid, maxPages 10)` | `Pages[{Page, TotalSlots, UsedSlots}]`: one page per file type, in the order tuning, liveries, layer groups, **photos**, replays (page 3 is the photos; page 1 opened MY PAINT JOBS). The screen shows `SLOTS AVAILABLE (Total - Used)/Total`; no slot count is in the game (the server decides) |
| `GetFileListForPage(ownerXuid, startAt, maxFiles, page)` | the player's files of that page, one at a time in practice (`maxFiles 1`), `totalFiles` for the grid. A file's **`Ordinal` is the photo's EnvironmentId**: the detail screen's LOCATION reads it (63 = Colorado; other values: MISSING CONTENT) |
| `GetFeaturedPhotos(10)`, `GetMostDownloadedPhotos(10)`, `GetTopRatedPhotos(10)` | BROWSE PHOTOS (PG PICKS and the other tabs), with previews |
| `SearchPhotoFiles(searchParams, maxFiles 8192)` | `searchResults[{Id, Rating, Price, DownloadCount, Ordinal}]`; a field of -1 is "any" |
| `GetFileHeavyWeightData`, `Purchase(fileId, price, maxPayloadSize, maxPreviewSize)` | a photo's data; `Purchase` answers the picture and preview (free) |
| `RemoveFile`, `ChangeFileTitle`, `ChangeFileDescription`, `SubmitFileRating`, `IncrementViews`, `GetStats` | REMOVE (the screen's list stays stale until it is reopened), CHANGE DESCRIPTION, the star rating, views, MY DETAILS |

VIEW downloads the picture with `Content.DownloadContent(fileId)`, so a shared photo is served to
anybody (other content only to its owner). The server keeps `STOREFRONT_SLOTS` = 25 shared
photos per player (`StorefrontPhoto.Create` fails with "storefront full" at 25; a REMOVE or
an upload that never finished, after an hour, frees a slot), the 100-files and 2 MB limits
of `LargeFileUpload` still apply, and the page, the lists and the search cover everybody's
finished photos. Seen in a game (a copy of a save, a local server): photo mode > PHOTO SHARE >
MY SHARED PHOTOS shows 25/25, UPLOAD PHOTO of a saved photo says "successfully uploaded", the
list then shows it (24/25, LOCATION Colorado), BROWSE PHOTOS shows it under PG PICKS, REMOVE
gives the slot back (25/25 on reopening). The thumbnails of the photo saved on a headless
session are black. Not seen: other players' photos (no second account), ratings, search
results with a match, `Purchase`, the 27 KB request (a photo of the player's own).

### Notifications (Message Center) (`server/forza_notifications.py`)

The Message Center is in the pause menu (START, MESSAGE CENTER; not in multiplayer). It
lists what `Xls Notifications.RetrieveForUser(nonce, maxResults 512, cbParameterBlockSize
0xC800)` (`sub_82AB3CF0`) returned: records `{id, xuidRecipient, notificationType,
hasThumbnail, isRead, deleteAction, expirationDate, cbParameters}` (0x22 bytes each in the
game) and, in `parameterBlock`, each one's parameters one after the other (cbParameters
bytes each), in the xtsw encoding of service records.

**When the game polls.** The notification manager (`sub_82AB7168`, ticked on a network
thread; state at +0x78) starts once the Message Center model (`sub_8283C328`, created by
`sub_82618A10` after the sign-in) registers with it. It takes the nonce, the poll interval
and up to 5 categories from `Logon2`'s `notificationNonce`, `notificationPollIntervalSeconds`
and `notificationCategories` (it does not call `StartNewSession` on this path), and polls
that many seconds later; after that every `nextRetrieveSeconds` of the last answer. With
the old interval of 600 s the first poll came 10 minutes after the sign-in. The server now
sends 15 s in `Logon2` and 60 s in the answers.

**Categories.** A notification's category is the high byte of its type without bit 15.
The Message Center keeps a tab per `Logon2` category (`type`, `name`, `description`) and
crashed (a null read in `sub_82706320`) when it opened with a notification whose category
had no tab. The server sends 1 News, 2 Account, 3 Gifts & Auctions, 4 Rivals, 5 Car Club;
the screen shows ALL MESSAGES and the tabs that have messages (LB/RB).

**Types.** The type picks the parameter record and the Message Center entry
(`sub_82ABD248`, which decodes the parameters with the record's member list and calls the
visitor at `0x82071A24`; the member lists are filled by the static initializers that branch
to `0x82A93090`, so their layouts can be read from the image without a running game):

| Type | Entry | Parameters |
| --- | --- | --- |
| 0x0100, 0x0102, 0x0200, 0x0300, 0x8401 | GENERAL MESSAGE | `Message` |
| 0x0101 | NEW DLC AVAILABLE | `Message, OfferId` |
| 0x0201 / 0x0202 | banned / unbanned | `BanExpireTime, BannedFromFeatureAreas` |
| 0x8300 - 0x8303 | auction outbid, sold, won, failed | `AuctionId, CarId` (+ `SalePrice` for sold); listed without title or text |
| 0x8304 | NEW GIFT (credits): A DOWNLOAD = `Gifting.RetrieveCreditsGift(GiftId)` | `GiftId, Amount, UserSuppliedReason, SenderXuid, SenderGamerTag` |
| 0x8305 | gift of a file (tuning, livery, photo...) | `GiftId, ItemId, GiftFileType, SenderXuid, SenderGamerTag, Reason` |
| 0x8306 | NEW GIFT (car): A DOWNLOAD = `RetrieveCarGift(GiftId)` | `GiftId, MakeId, ModelId, SenderXuid, SenderGamerTag, Reason` |
| 0x8307, 0x8308 | storefront out of stock, MONEY RECEIVED (A RECEIVE CREDITS) | file id/type/title; `Amount` |
| 0x8309 | profile import award | `awardAmount` |
| 0x8402 | RIVAL RECOMMENDATION (A SET WAYPOINT) | `TimeTrialId, localizedTimeTrialName, trackName, description, category` |
| 0x8403 | RIVAL CHALLENGE: "X beat you in the ... Horizon Rival Challenge on <date>! Their time, Their car, Track" (A SET WAYPOINT) | `senderXuid, senderGamertag, senderClanTag, senderClanRole, senderFlags, sentTime, timeTrialId, carId, localizedTimeTrialName, trackName, trackId, score, scoreType, category` |
| 0x0500 | CLUB REQUEST (A ACCEPT, Y CLUB HISTORY) | `requestorXuid, requestorGamertag, clanId` |
| 0x0501 | CLUB INVITE (A JOIN, Y CLUB INFO) | `sponsorXuid, clanId, clanName, clanTag, sponsorGamertag` |
| 0x0502 - 0x0508 | kick recommendation, kicked, demotion, MEMBERSHIP ACCEPTED, member quit, NEW CLUB MEMBER, CLUB MEMBER PROMOTED | gamertags, XUIDs, `clanName, clanTag` (promotion: `newRole`) |
| 0x8501 | shared car added/removed | `sharedCarAction, playerXuid, playerGamertag, MakeName, ModelName, perfIndex, Year` |
| 0x0000, 0x8000, 0x8001 | permissions changed, badge granted, set credits (no entry) | `Flags`; `BadgeId`; `creditAmount, action` |

Reading a message calls `Notifications.MarkRead(id)`, X DELETE and a collected gift
`Clear(id)`; the server keeps the read flag and drops cleared ones
(`DATA_DIR/notifications.json`, per recipient, 30 days).

**What sends them.** `Scoreboard.NotifyRival` (a player beat their rival's time): RIVAL
CHALLENGE to the rival, from the player's best on that board. `Gifting.SendCarGift`: NEW
GIFT to the recipient. Car clubs: a request to join a closed club goes to its leaders and
co-leaders, an approval to the player (MEMBERSHIP ACCEPTED), an invitation to the invitee
(its id is also accepted as `AcceptClanInvite`'s `invitationId`), a new member, a member
who quit, a removal, a promotion or demotion, a kick recommendation to the members it
concerns. Auctions: outbid, sold, won, failed (the Auction House is not reachable, below).
The server's operator can send messages and gifts from the server's machine:
`GET /notify?to=<gamertag>&type=0x100&Message=...` (any type and its fields) and
`GET /gift?to=<gamertag>&credits=25000&message=...` or `&car=<Data_Car id>` (a stock car).
`--test-notifications` queues a sample of each kind, 10,000 credits and a 370Z for every
player at their first sign-in.

Tested (one console, DriverA, a copy of the career save): all samples listed in the
Message Center (ALL MESSAGES (16), NEWS (2)) with their text, e.g. "[HZN] Turn 10 has
been promoted to the rank of CO-LEADER in the Horizon Kings club", "Turn 10 sent you an
invite to join the Horizon Kings club. Would you like to join?", "Turn 10 beat you in the
GoPro Millennia Masters Horizon Rival Challenge on 10/11/2026 09:21:48 PM! Their time:
01:33.294, Their car: LP700-4"; "Forza Live has gifted you a 370Z": DOWNLOAD called
`RetrieveCarGift`, the game saved ("Saving content") and cleared the message; the credits
gift's DOWNLOAD called `RetrieveCreditsGift` and the save's credits went from 642,996 to
652,996.

## Testing these services

`--dump-requests` saves every call under `DATA_DIR/requests`; `tools/xtsw_decode.py -n 20
DATA_DIR` decodes the last ones with `server/forza_xtsw_all.py` (all 292 records). The
career tests used a copy of the user's Xenia save (`tools/play.sh --xenia`, a copy per
instance under its `XDG_DATA_HOME`), which starts at the GoPro Millennia Masters sign-up;
the map (BACK) fast-travels (X on a festival icon) to Race Central, the Car Club, the
Autoshow and the Paintshop. `tools/autodrive.py` drives a race for a test. The session
scripts of these tests (`solo2.sh`: server + one or two consoles to single player; `pg.sh`:
two consoles into Playground Games; `seed.py`: a score from a fake player, optionally with
a ghost; `ut_*.py`: in-process unit tests of the services) were scratch files and are not
in the repo.

## Without a server answer (#63, 2026-10-15)

A Forza Live server that never answers (TU4, `FORZA_SERVER` at a closed port, and at a
UDP port that swallows packets) does not hang the game: the boot waits 3 s for the
welcome ("no answer ... still trying"), title servers enumerate empty, stats reads answer
defaults after 0.5 s and matchmaking searches answer no sessions at once. Single Player
loads free roam; Multiplayer shows "Server Unavailable" (no XP), Find Game offers the
default playlist (Quick Match), which hosts its own session and waits for players.
`joining session 0000000000000000 (flags 00000702)` is not a join: it is the game's own
single-player free-roam presence session (no host flag, invites and joins disabled, empty
XSESSION_INFO), logged on every free-roam load, with or without a server. The player's
endless loading (Windows, v0.1.6) stopped right after it, before the facade unload that
follows within a second normally; that run had crashed earlier on a guest read of 0x4 with
registers like a broken `longjmp` landing (lr = r31 = mid-function `0x824469B8`), the class
of the libjpeg crash fixed after v0.1.6 (CRT setjmp/longjmp as host calls, PROJECT.md
2026-10-15). Not confirmed: whether that load decodes a JPEG (the save's car pictures).

## State and next steps (Message Center work, 2026-10-12)

Done and tested in the game: notifications (format, categories, poll timing), operator
gifts (credits and a stock car collected), club invitation and join request with two
consoles, Monthly Rivals listing and loading a trial, and **RIVAL CHALLENGE from a real
`NotifyRival`**: two consoles, board 199 with A's score at 200 s and B's entry removed; B
(at the Millennia Masters sign-up) X (ticket), X (RIVALS), A (rival), A (car), X (race
anyway), A (START RIVALS), `autodrive.py b 240` with `AD_PATH_LOAD` beat A in one lap
(1:53.281), A (finish); the game sent `SubmitScoreEx` and `NotifyRival`, A's next poll
(about 2 s later) put 1 on MESSAGE CENTER in the pause menu, and it read "[HZN] DriverB
beat you in the GoPro Millennia Masters Horizon Rival Challenge on <date>! Their time:
01:53.281, Their car: LP700-4, Track: Clear Springs Circuit, Would you like to try to beat
them now?". First attempt, no
retries needed.
Session scripts: `solo2.sh` (`START="a b"` for two consoles, `SERVER_ARGS=...`),
`ctl.sh`, `seed.py` / `seed_tt.py` (time trial scores), `ut_notify.py` (unit test of the
notification flows), all scratch files not kept in the repo.

## Not done

- The other services (the design storefront for liveries, tuning, layer groups and replays,
  DLC metadata) get empty answers; only the photo storefront is kept (above).
  Existing local design/vinyl saves can now be inspected and exported with
  `tools/ugc_catalog.py` ([preservation workflow](ugc-preservation.md)); these bundles
  preserve encrypted payloads and attribution but are not yet installable or served online.
- Monthly Rivals: a time trial raced to its end (score, ghost, bounty) was not tried; the
  server has two sample trials and no tool to edit them.
- Car gifts between players: the game's CAR SELECT (Dak's Garage, CHANGE CAR) offers GET
  IN CAR, VIEW CAR, SHARE CAR (the club garage) and VIEW HISTORY, no GIFT CAR, so only the
  server's operator gifts (`/gift`) reach the Message Center; the `SendCarGift` path is
  server-side and unit-tested only.
- Notifications: auction ones are listed without text (no Auction House). The Message
  Center starts empty at each new session (a race loading starts one) until its first
  poll, which the server asks for 15 s later (`StartNewSession`'s `nextRetrieveSeconds`).
- Car clubs: creating one in the game (keyboard), club rankings, borrowing a shared car
  (`DownloadSharedCar`) were not tried.
- Playground Games: Cat & Mouse was driven to its end with the other player parked (#13);
  a cat chasing the mouse, and four or more players, were not tried.
- `DLCMetadata.GetStringTableInfo` / `Content.DownloadContent` (DLC string tables and
  thumbnails served by Turn 10): none. `GetStringTableInfo` answers `Id` -1, the game's
  "no table" (#79). The empty answer (`Id` 0, no `Path`, `Size` 0) made the game download
  content 0 and save it as `marketplace:\` + an empty name in its "marketplace" save; the
  open fails (`[NtCreateFile] FAILED: path='marketplace:\' -> 0xc00000ba`, three tries),
  the marketplace manager (`0x833F4CEC`) goes to state 3, and `sub_82753DB0`, which the game
  runs on the way into Horizon Rally and from some free-roam menus, answers "MARKETPLACE
  ERROR: There was not enough space for the marketplace save file". The SDK's device free
  space is a constant (3 GB of a 20 GB dummy HDD) and plays no part.
