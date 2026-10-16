// forza - what other Forza Live code needs from the virtual network (net.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace forza {

// The virtual network is on (FORZA_NET or a Forza Live server).
bool NetworkOn();

// The Forza Live server's IPv4 address (host order) and the TCP port of its title
// servers (HTTP). False without FORZA_SERVER, or before the server welcomed us.
// TCP connections the game opens to this address are sent to that port.
bool LiveServerHttp(uint32_t& ip, uint16_t& port);

// Another player in our room on the Forza Live server: a "friend" of a Live player.
struct LiveFriend {
  uint32_t id = 0;  // server id
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t state = 0;           // X_ONLINE_FRIENDSTATE flags it reported
  uint8_t session[0x3C] = {};   // XSESSION_INFO of its joinable session, zeros if none
  std::string status;           // presence line
  std::vector<uint8_t> extra;   // its hosted session's slots and QoS data (live_xmsg.cpp)
};

// The other players in the room, as of the server's last answer (refreshed every 3 s).
std::vector<LiveFriend> LiveRoster();

// Our presence: X_ONLINE_FRIENDSTATE flags, the XSESSION_INFO friends can join (null:
// none) and a status line. Sent to the server now and with every keep-alive.
void LiveSetPresence(uint32_t state, const uint8_t* session_info, const std::string& status,
                     const std::vector<uint8_t>& extra = {});

// This instance's XNADDR (36 bytes, guest layout).
void LiveSelfXnAddr(uint8_t* xnaddr);

// Game invites through the server: send one to a player in the room (with our joinable
// session), and take the one the player accepted (false when none is waiting;
// live_invites.cpp).
struct LiveInvite {
  uint64_t inviter_xuid = 0;
  std::string inviter;
  uint8_t session[0x3C] = {};
};
void LiveSendInvite(uint64_t xuid, const uint8_t* session_info);
bool LiveTakeInvite(LiveInvite& out);

// The invite a previous run of the game kept as launch data when it restarted itself to
// join (X_INVITE_INFO, 0x54 bytes; xam_loader.cpp), once; false when there is none.
bool LoaderTakeBootInvite(uint8_t* out);

// Called by the network thread when an invite arrives: the player is asked whether to
// join (live_invites.cpp, #90).
void LiveInviteReceived(const LiveInvite& invite);

// The invite picker (live_invites.cpp): the player searches the online players, picks
// whom to invite to `session_info` (XSESSION_INFO) and sends. `from` names the caller in
// the log.
void LiveOpenInvitePicker(const uint8_t* session_info, const char* from);

// Player 1's pad for the picker and the invite prompt (XamInputGetState): returns the
// buttons to hide from the game; `block_all`: the game gets no input at all.
uint16_t LiveInvitePad(uint16_t buttons, int left_y, bool& block_all);

// Matchmaking sessions on the server (live_xmsg.cpp; RecompiledHaloReach's
// docs/online_plan.md section 5.4): publish ours or search for a playlist's (bodies as
// forza_live_server.py documents); the network thread hands the server's answers to
// LiveMatchResults. The last published records (ours, our standby) are sent again with
// every keep-alive, so a lost datagram or a server restart does not hide our session.
// False when nothing was sent (the server has not welcomed us).
bool LiveSendMatch(bool search, const std::vector<uint8_t>& body);
void LiveMatchResults(const uint8_t* body, size_t size);

// The playlist our last matchmaking search was for (Find Game: the hopper id; 0 before the
// first search). Hooks use it to tell the Horizon World from other playlists.
uint32_t LiveMatchPlaylist();

// Called about every second by the network thread: reports host changes of the
// matchmaking session we joined (live_xmsg.cpp).
void LiveMatchTick();

// WORLD_EVENT from the server (net.cpp): an event waits for the Horizon World we host
// (hooks/world_events.cpp takes it).
void LiveWorldEvent(const uint8_t* body, size_t size);

// Head-to-head challenges (hooks/world_challenge.cpp, server/forza_challenge.py): send a
// CHALLENGE message to the server; the network thread hands the server's to LiveChallenge.
void LiveSendChallenge(const std::vector<uint8_t>& body);
void LiveChallenge(const uint8_t* body, size_t size);

// Xbox LIVE stats on the server (live_xmsg.cpp, server/forza_stats.py): send a STATS message
// (a write or a read); the network thread hands the server's rows to LiveStatsRows.
void LiveSendStats(const std::vector<uint8_t>& body);
void LiveStatsRows(const uint8_t* body, size_t size);

// The online game's "this console hosts the session" flag: 1, 0, or -1 without an online
// game (hooks/world_drop_in.cpp).
int OnlineGameIsHost();

// Called by the network thread when the roster changed (live_xmsg.cpp): tells the
// game its friends' presence changed, or that friends came or went.
void LiveRosterChanged(bool membership_changed);

}  // namespace forza
