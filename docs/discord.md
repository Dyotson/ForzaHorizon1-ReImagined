# Discord Rich Presence

Status display for #146. **Settings > Online > Discord > Share activity on Discord**
is off by default and applies at the next game launch. The Discord desktop client
must be running as the same OS user; Discord in a browser cannot receive local RPC.
Playing offline in Forza Live settings still supports Discord status display.

The game publishes its activity (free roam, festival/street/showcase races, rivals,
PR stunts, autoshow, paint shop, workshop, race central, car club, lobby and online
game type), elapsed game-session time, and session-local player counts when known.
It does not publish gamertags, profile IDs, save contents, server addresses, session
keys, join secrets or precise locations. The connection runs on worker threads;
game callbacks only copy small pieces of state. Updates are coalesced to at most
one every five seconds. Reconnect sends the latest activity. Closing the game
closes the connection and clears its activity; a crash closes the pipe as well.

## One-time application setup

This project does not yet have an official Discord application ID. Status display
therefore needs an ID configured before it can connect; an empty or invalid ID
logs a warning and leaves the game running normally.

1. In the [Discord Developer Portal](https://discord.com/developers/applications),
   create an application named **Forza Horizon 1: ReImagined**. That application's
   name and icon are what Discord displays as the game identity.
2. Copy the public **Application ID** from General Information to
   **Settings > Advanced > Discord setup > Discord application ID**.
3. Enable sharing under Online and launch the game. Discord's own activity privacy
   settings must also allow sharing. No bot, OAuth login, token or client secret is used.

For a direct launch, set `discord_presence = true` and
`discord_application_id = "YOUR_PUBLIC_APPLICATION_ID"` in `forza.toml`.
Missing/stopped Discord is optional: the game continues and retries in the background.
The log uses `DISCORD:` for connection and activity updates; it does not log Discord
account details or server-provided error text.

Linux also searches Discord's Flatpak runtime directories for stable and Canary.
The launcher Flatpak grants access to the standard desktop IPC socket and these
specific runtime directories. There is no network listener. A desktop Discord
using a nonzero IPC slot inside the game Flatpak may need an extra narrow permission,
for example `flatpak override --user --filesystem=xdg-run/discord-ipc-1
io.github.dyotson.ForzaHorizon1ReImagined`.

## Implementation and validation

`platform/discord_presence.cpp` owns the service, `discord_model.h` formats only
allowlisted primary-user Xbox context IDs, and `kernel/live_xmsg.cpp` observes XGI
`XUserSetContext` (`0xFB/0xB0006`) before its Forza Live sign-in gate. The presence
context is `0x8001`; online mode `0x12`, free-roam variant `0x15` and event variant
`0x16`. Mode values were checked against the player's own executable metadata;
no game strings or metadata are bundled. Party counts come from the newest local
hosted/joined presence session, not the server's global online-player roster.

The native client is Discord's [MIT-licensed classic RPC implementation](https://github.com/discord/discord-rpc),
which Discord has archived in favor of its Social SDK. `cmake/discord.cmake` pins and
hash-checks the source archives and builds a static library, using a newer pinned
RapidJSON revision for current compiler compatibility. The native runtime retains
both license notices under `licenses/`, including after the launcher cleans the build
tree. The Linux transport supports Flatpak socket paths and queues fragmented reads.
Protocol reference: [Discord's RPC-only Rich Presence guide](https://github.com/discord/discord-rpc/blob/master/documentation/hard-mode.md).

Build isolated tests (no game files or SDK required):

```sh
cmake -S forza-recomp/tests/discord -B out/discord -DCMAKE_BUILD_TYPE=Release
cmake --build out/discord --config Release
ctest --test-dir out/discord -C Release --output-on-failure
python tools/test_discord_rpc.py --exe out/discord/forza -v
# Windows multi-config generator: --exe out/discord/Release/forza.exe
```

Tests change the pipe prefix to a private fixture namespace and never connect to real
Discord. They cover model states, ID validation, framed handshakes, fragmented messages,
ping/pong, activity payloads, reconnect/resend and explicit clear. Windows and Linux run
the same tests in `discord-presence-tests.yml`.

Joining friends through Discord is a later phase of #146. This implementation advertises
no Join button or join secret. Current car names are also not part of the Xbox presence
contexts observed here; this version displays the activity and session counts.
