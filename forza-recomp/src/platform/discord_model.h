// Pure activity formatting; no SDK or Discord dependency (tests/discord_probe.cpp).
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace forza::discord {
inline bool ValidApplicationId(std::string_view id) {
  uint64_t number = 0;
  if (id.size() < 17 || id.size() > 20 || id.front() == '0') return false;
  auto result = std::from_chars(id.data(), id.data() + id.size(), number);
  return result.ec == std::errc{} && result.ptr == id.data() + id.size() && number != 0;
}

struct Activity {
  std::string details, state;
  int count = 0, capacity = 0;
  bool operator==(const Activity&) const = default;
};

struct Model {
  uint32_t presence = 0, online_mode = 0, region = 0, event = 0;
  uint32_t count = 0, capacity = 0;
  bool Context(uint32_t id, uint32_t value) {
    switch (id) {
      case 0x8001: presence = value; return true;
      case 0x12: online_mode = value; return true;
      case 0x15: region = value; return true;
      case 0x16: event = value; return true;
      default: return false;
    }
  }
  Activity Format() const {
    Activity a{"At the main menu", "Horizon Festival"};
    switch (presence) {
      case 0: break;
      case 1: a = {region == 1 ? "Preparing for Horizon Rally" : "Exploring Colorado", "Free roam"}; break;
      case 2: a = {event ? "Racing in Horizon Rally" : "Racing in a festival event", "Single player"}; break;
      case 3: a = {"Racing on the streets", "Single player"}; break;
      case 4: a = {"Competing in a showcase", "Single player"}; break;
      case 5: a = {"Taking on a PR stunt", "Horizon Outpost"}; break;
      case 6: a = {"Challenging a rival", "Rivals"}; break;
      case 7: a = {"Racing a rival", "Head to head"}; break;
      case 8: a = {"Browsing cars", "Autoshow"}; break;
      case 9: a = {"Choosing an event", "Race Central"}; break;
      case 10: a = {"Creating a design", "Paint shop"}; break;
      case 11: a = {"Tuning a car", "Workshop"}; break;
      case 12: a = {"Browsing add-ons", "Marketplace"}; break;
      case 13: a = {"Visiting the car club", "Horizon Festival"}; break;
      case 14: a = {"In an online lobby", "Multiplayer"}; break;
      case 15:
        switch (online_mode) {
          case 0: a = {"Racing online", "Multiplayer"}; break;
          case 1: a = {"Playing Infected", "Playground Games"}; break;
          case 2: a = {"Playing King", "Playground Games"}; break;
          case 3: a = {"Playing Cat and Mouse", "Playground Games"}; break;
          case 4: a = {"Exploring Colorado online", "Online free roam"}; break;
          case 5: a = {"Racing in Horizon Rally online", "Multiplayer"}; break;
          default: a = {"Playing online", "Multiplayer"}; break;
        }
        break;
      default: a = {"Playing Forza Horizon", "Horizon Festival"}; break;
    }
    if ((presence == 14 || presence == 15) && capacity > 0 && capacity <= 64 && count > 0 && count <= capacity) {
      a.count = int(count);
      a.capacity = int(capacity);
      a.state += " | " + std::to_string(count) + "/" + std::to_string(capacity) + " players";
    }
    return a;
  }
};
}  // namespace forza::discord
