// Head-to-head text, published by the network/input threads and consumed by the
// game's native notification control on its UI thread. No guest UI calls here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace forza::challenge_hud {
enum class Actions { kNone, kChallenge, kAnswer };
struct Message {
  Actions actions = Actions::kNone;
  std::string title, first, second;
  int64_t until = 0;
  uint64_t revision = 0;
};

struct Lines { std::string headline, caption; };
inline Lines NativeLines(const Message& message) {
  // The general notification displays LINE_ONE as its headline and TITLE as
  // its small caption. LINE_TWO belongs to the sponsor variants and is hidden
  // here. Give instructions/countdown/distance that visible caption slot.
  if (message.actions == Actions::kAnswer)
    return {message.first, "        Accept                   Decline " + message.second};
  if (message.actions == Actions::kChallenge)
    return {message.first, "        Challenge"};
  return {message.first.empty() ? message.title : message.first,
          message.second.empty() ? message.title : message.second};
}

class Mailbox {
 public:
  void Show(std::string title, std::string body, double seconds, int64_t now,
            Actions actions = Actions::kNone) {
    if (!std::isfinite(seconds) || seconds <= 0) {
      Clear();
      return;
    }
    const auto newline = body.find('\n');
    std::string first = body.substr(0, newline);
    std::string second = newline == std::string::npos ? "" : body.substr(newline + 1);
    std::replace(second.begin(), second.end(), '\n', ' ');
    std::lock_guard lock(mutex_);
    // Refresh the deadline without rebinding identical text or replaying SHOW.
    if (message_.title != title || message_.first != first || message_.second != second ||
        message_.actions != actions)
      ++message_.revision;
    message_.actions = actions;
    message_.title = std::move(title);
    message_.first = std::move(first);
    message_.second = std::move(second);
    message_.until = now + int64_t(std::min(seconds, 60.0) * 1000);
  }
  void Clear() {
    std::lock_guard lock(mutex_);
    message_.until = 0;
  }
  std::optional<Message> Read(int64_t now) {
    std::lock_guard lock(mutex_);
    return now < message_.until ? std::optional(message_) : std::nullopt;
  }

 private:
  std::mutex mutex_;
  Message message_;
};

// The game retains UTF-16 text. Bound input, replace malformed UTF-8, and strip
// embedded NUL/control characters so player names cannot truncate a prompt.
inline std::u16string Text(std::string_view input, size_t limit = 120) {
  std::u16string out;
  for (size_t i = 0; i < input.size() && out.size() < limit;) {
    uint32_t c = uint8_t(input[i++]);
    unsigned more = 0;
    uint32_t minimum = 0;
    if (c >= 0xC2 && c <= 0xDF) { c &= 0x1F; more = 1; minimum = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { c &= 0x0F; more = 2; minimum = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { c &= 7; more = 3; minimum = 0x10000; }
    else if (c >= 0x80) c = 0xFFFD;
    if (more) {
      unsigned read = 0;
      while (read < more && i < input.size() && (uint8_t(input[i]) & 0xC0) == 0x80) {
        c = (c << 6) | (uint8_t(input[i++]) & 0x3F);
        ++read;
      }
      if (read != more || c < minimum || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        c = 0xFFFD;
    }
    if (c < 0x20 || c == 0x7F) c = ' ';
    if (c > 0xFFFF) {
      if (out.size() + 2 > limit) break;
      c -= 0x10000;
      out.push_back(char16_t(0xD800 + (c >> 10)));
      out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
    } else out.push_back(char16_t(c));
  }
  return out;
}
}  // namespace forza::challenge_hud

namespace forza {
void ChallengeHudShow(const std::string& title, const std::string& body,
                      double seconds, bool log = true,
                      challenge_hud::Actions actions = challenge_hud::Actions::kNone);
void ChallengeHudClear();
}  // namespace forza
