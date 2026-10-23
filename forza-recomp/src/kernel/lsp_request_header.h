// forza - one header line added to every HTTP request the game sends to its title servers.
//
// The Forza Live server takes a title service call's player from the X-UserId header the
// game writes, which anyone can write. Our game adds its Live identity's friends key
// (X-Friends-Key, the secret the server bound to the XUID at the first HELLO; the launcher's
// shader list upload proves itself the same way), so the server can tell the player's own
// calls from someone else's (server/forza_live_lsp.py verify_caller). The game's requests
// are HTTP/1.1 with Content-Length over one kept-alive connection (docs/title_services.md);
// the head of each request is held until it is complete, the line goes in before its blank
// line, and the body passes through. Header-only so tools/test_lsp_request_header.cpp can
// test it without the runtime.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace forza {

class LspRequestHeader {
 public:
  static constexpr size_t kMaxHead = 64 * 1024;

  // `line`: a complete header line, "Name: value\r\n".
  explicit LspRequestHeader(std::string line) : line_(std::move(line)) {
    const size_t colon = line_.find(':');
    name_ = Lower(line_.substr(0, colon == std::string::npos ? 0 : colon));
  }

  // The bytes to send for these bytes of the request stream: everything except a request
  // head that is not complete yet, which is held for the next call.
  std::string Feed(const uint8_t* data, size_t size) {
    std::string out;
    size_t i = 0;
    while (i < size) {
      if (passthrough_) {
        out.append(reinterpret_cast<const char*>(data) + i, size - i);
        break;
      }
      if (body_left_ > 0) {
        const size_t take = size_t(std::min<uint64_t>(body_left_, size - i));
        out.append(reinterpret_cast<const char*>(data) + i, take);
        i += take;
        body_left_ -= take;
        continue;
      }
      const size_t search = head_.size() >= 3 ? head_.size() - 3 : 0;
      head_.append(reinterpret_cast<const char*>(data) + i, size - i);
      const size_t end = head_.find("\r\n\r\n", search);
      if (end == std::string::npos) {
        if (head_.size() > kMaxHead) {  // not HTTP as we know it: leave the stream alone
          out += head_;
          head_.clear();
          passthrough_ = true;
        }
        break;
      }
      const size_t head_size = end + 4;
      const size_t rest = head_.size() - head_size;  // after the head, from this call's data
      std::string head = head_.substr(0, head_size);
      head_.clear();
      i = size - rest;
      if (!Has(head, name_)) head.insert(head_size - 2, line_);
      body_left_ = ContentLength(head);
      out += head;
    }
    return out;
  }

  // Bytes of a request head held back (tests).
  size_t Pending() const { return head_.size(); }

 private:
  static std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  // Whether the head has a header of this (lower-case) name.
  static bool Has(const std::string& head, const std::string& name) {
    const std::string lower = Lower(head);
    return !name.empty() && lower.find("\r\n" + name + ":") != std::string::npos;
  }

  static uint64_t ContentLength(const std::string& head) {
    const std::string lower = Lower(head);
    const size_t at = lower.find("\r\ncontent-length:");
    if (at == std::string::npos) return 0;
    uint64_t value = 0;
    for (size_t p = at + 17; p < lower.size(); ++p) {
      const char c = lower[p];
      if (c == ' ' || c == '\t') continue;
      if (c < '0' || c > '9') break;
      value = value * 10 + uint64_t(c - '0');
      if (value > (uint64_t(1) << 40)) break;
    }
    return value;
  }

  std::string line_, name_;
  std::string head_;
  uint64_t body_left_ = 0;
  bool passthrough_ = false;
};

}  // namespace forza
