# Server community events

Community events record attendance in a named server session and let an organizer
approve unicorn rewards. An award creates a permanent multiplayer unlock for that
server. Garage ownership, car delivery and multiplayer permission are separate.
Single-player cars are unrestricted. This work is independent of launcher PR #191.

## Start the service

Use the normal Forza Live server with its title-service port enabled:

```sh
python server/forza_live_server.py --http-port 21201 --event-unlocks
```

`--event-unlocks` is opt-in. It requires the updated TU4 client capability and
blocks unicorn selection until a current enrolled reward snapshot permits it.
Without that flag, existing car eligibility remains unchanged and older clients
continue to connect. Base/disc builds do not advertise the unvalidated TU4 hook.

The organizer tool uses the existing admin boundary: direct loopback, or
`FORZA_ADMIN_TOKEN` through the `X-Admin-Token` header. Remote operators should use
HTTPS through their existing proxy. The tool never puts the token in a URL and
refuses redirects. Browser-origin requests and form submissions cannot mutate
these endpoints. Keep the admin token out of client configuration.

## Run an event

1. Read `/status` for the server's current player XUID and hosted/joined session ID.
   Confirm the player with them, then explicitly enroll their currently verified
   friends credential:

   ```sh
   python server/event_admin.py enroll 0009000000000001
   ```

   A different key cannot inherit the enrolled player's grants. Enrollment will
   refuse ambiguous duplicate sign-ins or a restored registration that has not
   completed a fresh HELLO. It never silently replaces an existing enrollment.

2. Create an event JSON file. `starts` and `ends` are Unix seconds, at most seven
   days apart. Use an actual 16-digit session ID from `/status`, not the example:

   ```json
   {
     "id": "festival-night",
     "name": "Festival Night",
     "description": "Meet at the festival for a community cruise.",
     "activity": "Online free roam",
     "session": "0123456789abcdef",
     "starts": 1792526400,
     "ends": 1792530000,
     "organizer": "Festival organizer",
     "rewards": [1197]
   }
   ```

   ```sh
   python server/event_admin.py create event.json
   python server/event_admin.py start festival-night
   python server/event_admin.py attendance festival-night
   ```

   Enrolled updated clients poll through their current registration. Only polls
   observed inside the event window and matching its session count. Offline gaps
   are not filled. First observed attendance queues a native Message Center News
   announcement. `activity` describes the event; no automatic completion rule is
   inferred from it. Session membership is not proof of finishing a race.

3. Review attendance and approve a reward. Confirm which reward cars the attendee
   already owns. This first version does not inspect their complete local garage:

   ```sh
   python server/event_admin.py award festival-night 0009000000000001 --owned-models 1197
   ```

   That grants online use without creating a duplicate car. If all event reward
   models are confirmed missing, explicitly use `--owned-models none`. For a mix,
   list only the already-owned reward IDs, comma separated. Missing models receive
   one stock-car gift through Gifts & Auctions. An ownership confirmation is
   required; there is no unsafe default that gifts everything.

4. Close the event when finished. Existing awards remain valid:

   ```sh
   python server/event_admin.py close festival-night
   ```

   An organizer can revoke that event's grants for a player:

   ```sh
   python server/event_admin.py revoke festival-night 0009000000000001
   ```

   Another non-revoked event grant for the same model still permits it. Revoking
   cannot remove cars already delivered or owned in single player. Repeating an
   award request does not resurrect a revoked grant; use a separate legitimate
   event for a new award.

Pass `--server https://your-server.example` before the command for another server;
`--token-file` is an alternative to the organizer's environment variable.
`list` shows event definitions; `attendance` shows first/last observation and
observed seconds. The JSON award response includes grant IDs and delivery status.

## Persistence and messages

`events.sqlite3` holds enrolled credential fingerprints, events, participation,
per-event grants/revocations, and a durable delivery/notice outbox. Back up the
whole data directory together, including `friends.json`, `gifts.json` and
`notifications.json`; preserve SQLite consistently while the service is stopped
or with SQLite's backup API. Do not restore only the grants into an older delivery
store, which discards delivery receipts.

Approval is transactional and does not depend on successful delivery. A pending
car or announcement is retried on later server ticks. Gifts retain a delivery key
and notifications retain receipts after clearing/expiration, so a crash between
queueing and acknowledgement does not generate another car. Car delivery is
once per player/model across events. An already-owned confirmation also records
that delivery is unnecessary. Pending undelivered gifts are cancelled if no valid
grant remains; revocation does not erase a collected car.

The native dialog distinguishes unearned, checking, unavailable verification and
registration-required states. Normal class/event restrictions still apply even
after an unlock. The ordinary-car recovery path and single-player use are kept.
Award history appears in the existing Message Center News tab; actual missing
cars arrive separately in Gifts & Auctions. Messages use its normal polling
interval rather than replacing invite/chat HUD overlays.

## Protocol and practical limits

Forza Live feature `0x08`, datagram kind 24:

For capable clients, WELCOME appends the required-policy byte after its existing
HTTP-port extension. The client installs it before completing sign-in, so loss
of the separate policy packet cannot briefly allow a restricted selection.

* server policy: `>BHB` (0, epoch, required);
* client snapshot poll: `>BQQ` (1, registration token, fresh random nonce);
* server snapshot: `>BQIHIBBB` (2, nonce, revision, epoch, peer ID, enrolled,
  required, seven-bit allowed mask).

Mask order is 320, 326, 330, 1082, 1197, 1233, 1488. Polls run every three seconds
on the network thread; the native hook performs no network I/O. Replies must
match the outstanding nonce, current registration/epoch and nondecreasing revision,
arrive within five seconds and expire after twelve seconds. New registrations
clear cached permissions; a server that restores its registration IDs still has
to answer fresh polls before the cache expires. Ordinary models and single-player
contexts do not use this gate.

This deliberately reuses Forza Live's existing friends credential and UDP trust
boundary. The organizer pins a verified credential instead of trusting an XUID
header, but HELLO still carries that credential over the existing unencrypted
protocol. This is a cooperative community-server feature, not secure account
authentication against an on-path attacker or anti-cheat. Modified clients can
lie about session membership/cars or ignore the local gate. Remote-car admission,
join-in-progress, in-world changes and revocation of an already-driving car still
need independent multi-client/native validation before claiming full enforcement.
The first release does not automatically award race finishes, migrate grants
between servers, or recover an identity from a new key.

## Checks

```sh
python -m unittest discover -s server -p 'test_forza_*.py'
```

Standalone C++ assertions: `tests/unicorn_eligibility_test.cpp` and
`tests/event_rewards_test.cpp` under `forza-recomp/`. Keep assertions enabled.
The cache tests cover stale, reordered, duplicate, expired and wrong-registration
responses, revocation and all seven models. Server tests cover the organizer
boundary, enrollment, matching-session attendance, ownership confirmation,
retry/restart deduplication and persistence.
