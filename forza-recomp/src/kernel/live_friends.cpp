// forza - friends (#92): the friends list the Forza Live server keeps for us
// (server/forza_friends.py), the friends screen over the game, and its notifications.
//
// Games up to v0.1.14 counted every player in the server room as a friend. Now the server
// keeps each player's friends (requests by gamertag, accept or decline, remove, block) and
// this game, which says so in its HELLO (features 0x02 and its friends key), asks for its
// list every 10 s (every 3 s while the screen shows; the server also sends it after every
// change). With a list:
//
// - The game's friends list (XFriendsCreateEnumerator, XamUserAreUsersFriends;
//   live_xmsg.cpp) has only our friends: those in our room with their presence from the
//   roster (joinable sessions), friends online elsewhere without one, offline friends last.
// - The invite picker (live_invites.cpp) lists the friends online first, then everyone else
//   online; blocked players are not listed, and the server drops their invites to us.
// - The friends screen: F8 (live_friends_key) or holding the left stick press (unused by the
//   game; F on the keyboard with mnk_mode) opens it, B or Escape closes it. Tabs (LB/RB or
//   Left/Right): Friends, Requests, Add friend, Blocked. A or Enter on a player opens what
//   can be done (accept, decline, remove, block, unblock, take back a request). On Add
//   friend, typing searches the gamertags (players online here, and players the server
//   knows); with a controller, Y or A on the search line opens the on-screen keyboard
//   (live_osk.h). The game gets no input while it shows.
// - A friend request or an accepted one shows a short notification.
//
// Before the server sends a list (an older server), everyone in the room stays a friend.

#include "../platform/hud_overlay.h"
#include "identity.h"
#include "live.h"
#include "live_osk.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_STRING(live_friends_key, "F8", "Network/Forza Live",
                      "Key that opens the friends screen (a controller: hold the left stick "
                      "press)");

namespace forza {
namespace {

constexpr uint8_t kList = 1, kSearch = 2, kRequest = 3, kAccept = 4, kDecline = 5, kRemove = 6,
                  kBlock = 7, kUnblock = 8;
constexpr uint8_t kRList = 16, kRSame = 17, kRResults = 18, kRNotice = 19;
constexpr uint32_t kOnline = 0x1, kPlaying = 0x2;
constexpr int64_t kHoldMs = 500;        // the left stick press held this long opens the screen
constexpr int64_t kIdleMs = 120000;     // the screen closes after this without input
constexpr int64_t kNoticeMs = 6000;     // a notification shows this long
constexpr int64_t kStatusMs = 5000;     // a server message on the screen
constexpr int64_t kSearchDelayMs = 300; // typing pauses this long before a server search
constexpr size_t kRows = 8;
constexpr size_t kMaxSearch = 15 * 3;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return s;
}

uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t Load64(const uint8_t* p) { return uint64_t(Load32(p)) << 32 | Load32(p + 4); }

void Put64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 7; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i)));
}
void PutText(std::vector<uint8_t>& b, const std::string& s) {
  const size_t n = std::min<size_t>(s.size(), 255);
  b.push_back(uint8_t(n));
  b.insert(b.end(), s.begin(), s.begin() + n);
}
// A u8-length string at `pos`; false when it does not fit.
bool ReadText(const uint8_t* p, size_t n, size_t& pos, std::string& out) {
  if (pos >= n || pos + 1 + p[pos] > n) return false;
  out.assign(reinterpret_cast<const char*>(p + pos + 1), p[pos]);
  pos += 1 + p[pos];
  return true;
}

void SendOp(uint8_t op, uint64_t xuid, const std::string& name = {}, bool with_name = false) {
  std::vector<uint8_t> body{op};
  Put64(body, xuid);
  if (with_name) PutText(body, name);
  LiveSendFriend(body);
}

struct Result {
  uint64_t xuid = 0;
  std::string gamertag;
  bool online = false;
};

std::mutex mutex;
// --- the list (network thread writes) ---
bool active = false;
uint32_t version = 0;
std::vector<LiveRelation> relations, parts;
uint16_t parts_seen = 0;
uint32_t parts_version = 0;
std::atomic<int64_t> next_list{0};
// --- search results ---
uint16_t search_request = 0;
std::vector<Result> results;
// --- notifications ---
std::deque<std::string> notices;
int64_t notice_until = 0;

// --- the screen (game thread) ---
enum Tab { kFriends, kRequests, kAdd, kBlocked, kTabs };
enum Action { kDoAccept, kDoDecline, kDoRemove, kDoBlock, kDoUnblock, kDoRequest, kDoCancel,
              kDoBack };
struct Row {
  int kind = 1;  // 0 the search line, 1 a player, 2 "send a request to <search>"
  uint64_t xuid = 0;
  std::string gamertag, note;
  LiveRelationKind rel{};
  bool has_rel = false;
};
std::atomic<bool> screen{false};
std::atomic<bool> toggle_request{false};
Tab tab = kFriends;
size_t cursor[kTabs] = {};
std::string search;
int64_t search_changed = 0;
bool search_sent = true;
Osk osk;
bool menu = false;
size_t menu_cursor = 0;
Row menu_row;
std::vector<std::pair<std::string, Action>> menu_items;
std::string status;
int64_t status_until = 0, idle_until = 0, last_hud = 0;
bool dirty = true;
PadReader reader;
uint16_t latched = 0;  // buttons hidden from the game until released
int64_t ls_since = 0;

const LiveRelation* FindLocked(uint64_t xuid) {
  for (const auto& r : relations) {
    if (r.xuid == xuid) return &r;
  }
  return nullptr;
}

size_t CountLocked(LiveRelationKind kind) {
  size_t n = 0;
  for (const auto& r : relations) n += r.kind == kind;
  return n;
}

std::string RelNote(LiveRelationKind kind) {
  switch (kind) {
    case LiveRelationKind::kFriend: return "friend";
    case LiveRelationKind::kAsksUs: return "asked you";
    case LiveRelationKind::kWeAsked: return "request sent";
    default: return "blocked";
  }
}

// The rows of the current tab (mutex held).
std::vector<Row> RowsLocked() {
  std::vector<Row> rows;
  const auto roster = LiveRoster();
  auto in_room = [&](uint64_t xuid) {
    for (const auto& f : roster) {
      if (f.xuid == xuid) return &f;
    }
    return static_cast<const LiveFriend*>(nullptr);
  };
  switch (tab) {
    case kFriends:
      for (const auto& r : relations) {
        if (r.kind != LiveRelationKind::kFriend) continue;
        const LiveFriend* f = in_room(r.xuid);
        std::string note = !r.state ? "offline" : f ? "online" : "online (another room)";
        const std::string& line = f ? f->status : r.status;
        if (r.state && !line.empty()) note += ": " + line.substr(0, 40);
        rows.push_back({1, r.xuid, r.gamertag, note, r.kind, true});
      }
      break;
    case kRequests:
      for (const auto& r : relations) {
        if (r.kind == LiveRelationKind::kAsksUs) {
          rows.push_back({1, r.xuid, r.gamertag, "wants to be your friend", r.kind, true});
        }
      }
      for (const auto& r : relations) {
        if (r.kind == LiveRelationKind::kWeAsked) {
          rows.push_back({1, r.xuid, r.gamertag, "waiting for an answer", r.kind, true});
        }
      }
      break;
    case kBlocked:
      for (const auto& r : relations) {
        if (r.kind == LiveRelationKind::kBlocked) rows.push_back({1, r.xuid, r.gamertag, "", r.kind, true});
      }
      break;
    default: {
      rows.push_back({0, 0, "", "", {}, false});
      const std::string needle = Lower(search);
      std::unordered_set<uint64_t> seen{IdentityXuid()};
      bool exact = false;
      auto add = [&](uint64_t xuid, const std::string& tag, bool online) {
        if (!xuid || tag.empty() || !seen.insert(xuid).second) return;
        if (!needle.empty() && Lower(tag).find(needle) == std::string::npos) return;
        Row row{1, xuid, tag, online ? "online" : "offline", {}, false};
        if (const LiveRelation* r = FindLocked(xuid)) {
          row.rel = r->kind;
          row.has_rel = true;
          row.note += ", " + RelNote(r->kind);
        }
        exact = exact || Lower(tag) == needle;
        rows.push_back(row);
      };
      std::vector<const LiveFriend*> online;
      for (const auto& f : roster) online.push_back(&f);
      std::sort(online.begin(), online.end(), [](const LiveFriend* a, const LiveFriend* b) {
        return Lower(a->gamertag) < Lower(b->gamertag);
      });
      for (const LiveFriend* f : online) add(f->xuid, f->gamertag, true);
      if (!needle.empty()) {
        for (const auto& r : results) add(r.xuid, r.gamertag, r.online);
        if (!exact) rows.push_back({2, 0, search, "", {}, false});
      }
      break;
    }
  }
  return rows;
}

const char* TabName(Tab t) {
  switch (t) {
    case kFriends: return "Friends";
    case kRequests: return "Requests";
    case kAdd: return "Add friend";
    default: return "Blocked";
  }
}

void ShowScreenLocked(int64_t now) {
  last_hud = now;
  const bool log = dirty;
  dirty = false;
  std::string body;
  for (int t = 0; t < kTabs; ++t) {
    std::string name = TabName(Tab(t));
    size_t n = 0;
    if (t == kFriends) n = CountLocked(LiveRelationKind::kFriend);
    if (t == kRequests) n = CountLocked(LiveRelationKind::kAsksUs);
    if (t == kBlocked) n = CountLocked(LiveRelationKind::kBlocked);
    if (n) name += " " + std::to_string(n);
    body += t == tab ? "[" + name + "]   " : " " + name + "    ";
  }
  body += "\n";
  if (!active) body += "(waiting for the server's friends list)\n";
  std::string hint;
  if (menu) {
    body += menu_row.gamertag + ":\n";
    for (size_t i = 0; i < menu_items.size(); ++i) {
      body += (i == menu_cursor ? std::string(1, kHudHighlight) + "> " : std::string("  ")) +
              menu_items[i].first + "\n";
    }
    hint = "Up/Down: move   A or Enter: choose   B or Escape: back";
  } else if (osk.open) {
    body += "Search: " + search + "_\n" + osk.Render();
    hint = Osk::kHint;
  } else {
    const auto rows = RowsLocked();
    size_t& at = cursor[tab];
    if (at >= rows.size()) at = rows.empty() ? 0 : rows.size() - 1;
    if (rows.empty()) {
      body += tab == kFriends    ? "  No friends yet: add some on Add friend\n"
              : tab == kRequests ? "  No friend requests\n"
                                 : "  No one blocked\n";
    }
    size_t first = at > kRows / 2 ? at - kRows / 2 : 0;
    if (first + kRows > rows.size()) first = rows.size() > kRows ? rows.size() - kRows : 0;
    if (first > 0) body += "  ...\n";
    for (size_t i = first; i < rows.size() && i < first + kRows; ++i) {
      const Row& r = rows[i];
      std::string line;
      if (r.kind == 0) {
        line = "Search: " + search + "_";
      } else if (r.kind == 2) {
        line = "Send a friend request to \"" + r.gamertag + "\"";
      } else {
        line = r.gamertag + (r.note.empty() ? "" : "   " + r.note);
      }
      body += (i == at ? std::string(1, kHudHighlight) + "> " : std::string("  ")) + line + "\n";
    }
    if (first + kRows < rows.size()) body += "  ...\n";
    hint = tab == kAdd ? "Type a gamertag (Y: keyboard)   A or Enter: pick   LB/RB: tabs   "
                         "B or Escape: close"
                       : "Up/Down: move   A or Enter: pick   LB/RB or Left/Right: tabs   "
                         "B or Escape: close";
  }
  if (now < status_until) body += status + "\n";
  body += hint;
  HudShow("Friends", body, 2.5, log);
}

void OpenMenuLocked(const Row& row) {
  menu_items.clear();
  if (row.has_rel) {
    switch (row.rel) {
      case LiveRelationKind::kFriend:
        menu_items = {{"Remove friend", kDoRemove}, {"Block", kDoBlock}};
        break;
      case LiveRelationKind::kAsksUs:
        menu_items = {{"Accept", kDoAccept}, {"Decline", kDoDecline}, {"Block", kDoBlock}};
        break;
      case LiveRelationKind::kWeAsked:
        menu_items = {{"Take back the request", kDoCancel}, {"Block", kDoBlock}};
        break;
      default:
        menu_items = {{"Unblock", kDoUnblock}};
    }
  } else {
    menu_items = {{"Send a friend request", kDoRequest}, {"Block", kDoBlock}};
  }
  menu_items.push_back({"Back", kDoBack});
  menu_row = row;
  menu_cursor = 0;
  menu = true;
  dirty = true;
}

void DoLocked(Action a, const Row& row) {
  const char* what = "";
  switch (a) {
    case kDoAccept: SendOp(kAccept, row.xuid); what = "accept"; break;
    case kDoDecline: SendOp(kDecline, row.xuid); what = "decline"; break;
    case kDoCancel: SendOp(kDecline, row.xuid); what = "take back the request to"; break;
    case kDoRemove: SendOp(kRemove, row.xuid); what = "remove"; break;
    case kDoBlock: SendOp(kBlock, row.xuid, row.gamertag, true); what = "block"; break;
    case kDoUnblock: SendOp(kUnblock, row.xuid); what = "unblock"; break;
    case kDoRequest: SendOp(kRequest, row.xuid, row.gamertag, true); what = "ask"; break;
    default: return;
  }
  REXLOG_INFO("FORZA_LIVE: friends: {} {} ({:016X})", what, row.gamertag, row.xuid);
  next_list = 0;  // the server sends the new list too; ask anyway
}

void OpenScreenLocked(int64_t now) {
  screen = true;
  tab = CountLocked(LiveRelationKind::kAsksUs) ? kRequests : kFriends;
  menu = false;
  osk.open = false;
  reader.Reset();
  idle_until = now + kIdleMs;
  dirty = true;
  next_list = 0;
  HudWantKeys(true);
  HudTakeKeys();
  REXLOG_INFO("FORZA_LIVE: friends screen opened ({} friends, {} requests)",
              CountLocked(LiveRelationKind::kFriend), CountLocked(LiveRelationKind::kAsksUs));
}

void CloseScreenLocked(uint16_t buttons, const char* why) {
  screen = false;
  latched |= buttons;
  menu = false;
  osk.open = false;
  HudWantKeys(false);
  HudClear();
  REXLOG_INFO("FORZA_LIVE: friends screen closed ({})", why);
}

void SetSearchLocked(const std::string& s, int64_t now) {
  if (s == search) return;
  search = s;
  search_changed = now;
  search_sent = search.empty();
  results.clear();
  cursor[kAdd] = 0;
  dirty = true;
}

// The screen's input (mutex held).
void ScreenInputLocked(uint16_t buttons, int left_x, int left_y, int64_t now) {
  using namespace pad;
  const HudKeys keys = HudTakeKeys();
  uint16_t p = reader.Poll(buttons, left_x, left_y, keys.key_frames);
  if (keys.up) p |= kUp;
  if (keys.down) p |= kDown;
  const bool any_key = !keys.typed.empty() || keys.backspace || keys.enter || keys.escape ||
                       keys.up || keys.down || keys.left || keys.right;
  if (p || any_key) {
    idle_until = now + kIdleMs;
    dirty = true;
  }
  // Typing: the search (on Add friend).
  if (!keys.typed.empty() || keys.backspace) {
    std::string s = search;
    if (!menu) {
      if (tab != kAdd) tab = kAdd;
      s += keys.typed;
      for (int i = 0; i < keys.backspace; ++i) PopUtf8(s);
      if (s.size() > kMaxSearch) s.resize(kMaxSearch);
      SetSearchLocked(s, now);
    }
  }
  if (osk.open) {
    std::string s = search;
    if (keys.enter || keys.escape) p |= kB;
    osk.Press(p, s, kMaxSearch);
    SetSearchLocked(s, now);
    return;
  }
  if (menu) {
    const size_t n = menu_items.size();
    if (p & kUp) menu_cursor = (menu_cursor + n - 1) % n;
    if (p & kDown) menu_cursor = (menu_cursor + 1) % n;
    if ((p & kA) || keys.enter) {
      DoLocked(menu_items[menu_cursor].second, menu_row);
      menu = false;
    } else if ((p & kB) || keys.escape) {
      menu = false;
    }
    return;
  }
  if ((p & kB) || keys.escape) {
    CloseScreenLocked(buttons, "closed");
    return;
  }
  if ((p & (kRB | kRight)) || keys.right) tab = Tab((tab + 1) % kTabs);
  if ((p & (kLB | kLeft)) || keys.left) tab = Tab((tab + kTabs - 1) % kTabs);
  const auto rows = RowsLocked();
  size_t& at = cursor[tab];
  if (!rows.empty()) {
    if (p & kUp) at = (at + rows.size() - 1) % rows.size();
    if (p & kDown) at = (at + 1) % rows.size();
    at = std::min(at, rows.size() - 1);
  }
  if (p & kY) {
    tab = kAdd;
    cursor[kAdd] = 0;
    osk.Open();
    return;
  }
  if (((p & kA) || keys.enter) && at < rows.size()) {
    const Row& row = rows[at];
    if (row.kind == 0) {
      if (p & kA) osk.Open();
    } else if (row.kind == 2) {
      DoLocked(kDoRequest, row);
    } else {
      OpenMenuLocked(row);
    }
  }
}

}  // namespace

bool LiveFriendsActive() {
  std::lock_guard<std::mutex> lock(mutex);
  return active;
}

std::vector<LiveRelation> LiveRelations() {
  std::lock_guard<std::mutex> lock(mutex);
  return relations;
}

std::vector<LiveFriend> LiveFriendsList() {
  auto roster = LiveRoster();
  std::vector<LiveRelation> rels;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!active) return roster;
    rels = relations;
  }
  std::vector<LiveFriend> here, elsewhere, offline;
  for (const auto& r : rels) {
    if (r.kind != LiveRelationKind::kFriend) continue;
    auto it = std::find_if(roster.begin(), roster.end(),
                           [&](const LiveFriend& f) { return f.xuid == r.xuid; });
    if (it != roster.end()) {
      here.push_back(*it);
      continue;
    }
    LiveFriend f;
    f.xuid = r.xuid;
    f.gamertag = r.gamertag;
    f.state = r.state & (kOnline | kPlaying);  // no session to join from another room
    f.status = r.status;
    (f.state ? elsewhere : offline).push_back(std::move(f));
  }
  here.insert(here.end(), elsewhere.begin(), elsewhere.end());
  here.insert(here.end(), offline.begin(), offline.end());
  return here;
}

bool LiveIsFriend(uint64_t xuid) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (active) {
      const LiveRelation* r = FindLocked(xuid);
      return r && r->kind == LiveRelationKind::kFriend;
    }
  }
  for (const auto& f : LiveRoster()) {
    if (f.xuid == xuid) return true;
  }
  return false;
}

bool LiveIsBlocked(uint64_t xuid) {
  std::lock_guard<std::mutex> lock(mutex);
  const LiveRelation* r = FindLocked(xuid);
  return r && r->kind == LiveRelationKind::kBlocked;
}

void LiveFriendsTick() {
  if (!LiveSignin()) return;
  const int64_t now = NowMs();
  if (now < next_list) return;
  uint32_t v;
  {
    std::lock_guard<std::mutex> lock(mutex);
    v = version;
  }
  std::vector<uint8_t> body{kList, uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  if (LiveSendFriend(body)) next_list = now + (screen ? 3000 : 10000);
}

// The network thread: a FRIEND message from the server.
void LiveFriendMessage(const uint8_t* p, size_t n) {
  if (!n) return;
  bool friends_changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    switch (p[0]) {
      case kRList: {
        if (n < 11) return;
        const uint32_t v = Load32(p + 1);
        const uint16_t total = Load16(p + 5), first = Load16(p + 7), count = Load16(p + 9);
        if (first == 0) {
          parts.clear();
          parts_seen = 0;
          parts_version = v;
        }
        if (first != parts_seen || v != parts_version) return;  // a part went missing
        size_t pos = 11;
        for (uint16_t i = 0; i < count; ++i) {
          if (pos + 13 > n) return;
          LiveRelation r;
          r.xuid = Load64(p + pos);
          r.kind = LiveRelationKind(p[pos + 8]);
          r.state = Load32(p + pos + 9);
          pos += 13;
          if (!ReadText(p, n, pos, r.gamertag) || !ReadText(p, n, pos, r.status)) return;
          parts.push_back(std::move(r));
          ++parts_seen;
        }
        if (parts_seen < total) return;
        auto friend_set = [](const std::vector<LiveRelation>& v) {
          std::vector<uint64_t> out;
          for (const auto& r : v) {
            if (r.kind == LiveRelationKind::kFriend) out.push_back(r.xuid);
          }
          std::sort(out.begin(), out.end());
          return out;
        };
        friends_changed = !active || friend_set(parts) != friend_set(relations);
        if (!active) {
          REXLOG_INFO("FORZA_LIVE: friends list from the server: {} friends, {} requests",
                      friend_set(parts).size(), std::count_if(parts.begin(), parts.end(), [](const LiveRelation& r) {
                        return r.kind == LiveRelationKind::kAsksUs;
                      }));
        } else if (friends_changed) {
          REXLOG_INFO("FORZA_LIVE: friends: now {}", friend_set(parts).size());
        }
        relations = std::move(parts);
        parts.clear();
        parts_seen = 0;
        version = v;
        active = true;
        dirty = true;
        break;
      }
      case kRSame:
        active = true;
        break;
      case kRResults: {
        if (n < 4 || Load16(p + 1) != search_request) return;
        results.clear();
        size_t pos = 4;
        for (uint8_t i = 0; i < p[3]; ++i) {
          if (pos + 10 > n) break;
          Result r;
          r.xuid = Load64(p + pos);
          r.online = p[pos + 8] != 0;
          pos += 10;
          if (!ReadText(p, n, pos, r.gamertag)) break;
          results.push_back(std::move(r));
        }
        dirty = true;
        break;
      }
      case kRNotice: {
        if (n < 10) return;
        const uint8_t event = p[1];
        size_t pos = 10;
        std::string name, text;
        if (!ReadText(p, n, pos, name) || !ReadText(p, n, pos, text)) return;
        std::string line = event == 1   ? name + " sent you a friend request"
                           : event == 2 ? name + " accepted your friend request"
                                        : text;
        if (line.empty()) return;
        REXLOG_INFO("FORZA_LIVE: friends: {}", line);
        if (screen) {
          status = line;
          status_until = NowMs() + kStatusMs;
          dirty = true;
        } else if (notices.size() < 5) {
          if (event == 1) line += "\nAnswer it on the friends screen: " +
                                  REXCVAR_GET(live_friends_key) + " or hold the left stick";
          notices.push_back(line);
        }
        next_list = 0;
        break;
      }
      default:
        return;
    }
  }
  if (friends_changed) LiveRosterChanged(true);  // the game's friends list changed
}

bool LiveFriendsScreenOpen() { return screen; }

void InstallFriendsKey() {
  const std::string key = REXCVAR_GET(live_friends_key);
  if (key.empty()) return;
  rex::ui::RegisterBind("bind_friends", key, "Open the Forza Live friends screen",
                        [] { toggle_request = true; });
}

uint16_t LiveFriendsPad(uint16_t buttons, int left_x, int left_y, bool& block_all) {
  using namespace pad;
  block_all = false;
  const bool busy = LiveInviteBusy();  // before our lock: that one takes the invites' lock
  std::lock_guard<std::mutex> lock(mutex);
  latched &= buttons;
  if (!LiveSignin()) return latched;
  const int64_t now = NowMs();
  const bool toggle = toggle_request.exchange(false);
  if (screen) {
    if (toggle) {
      CloseScreenLocked(buttons, "key");
      return latched;
    }
    ScreenInputLocked(buttons, left_x, left_y, now);
    if (!screen) return latched;
    if (now >= idle_until) {
      CloseScreenLocked(buttons, "no input");
      return latched;
    }
    // A server search once the typing pauses.
    if (!search_sent && now - search_changed >= kSearchDelayMs) {
      search_sent = true;
      std::vector<uint8_t> body{kSearch, uint8_t(++search_request >> 8), uint8_t(search_request)};
      PutText(body, search);
      LiveSendFriend(body);
    }
    if (dirty || now - last_hud >= 500) ShowScreenLocked(now);
    block_all = true;
    return 0xFFFF;
  }
  // Closed: the left stick press held (or the key) opens it.
  if (buttons & kLS) {
    if (!ls_since) ls_since = now;
  } else {
    ls_since = 0;
  }
  if (!busy && (toggle || (ls_since && now - ls_since >= kHoldMs && !(latched & kLS)))) {
    ls_since = 0;
    OpenScreenLocked(now);
    ShowScreenLocked(now);
    latched |= buttons;
    block_all = true;
    return 0xFFFF;
  }
  if (!busy && !notices.empty() && now >= notice_until) {
    HudShow("Friends", notices.front(), kNoticeMs / 1000.0);
    notices.pop_front();
    notice_until = now + kNoticeMs;
  }
  return latched;
}

}  // namespace forza
