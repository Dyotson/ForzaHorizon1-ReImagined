// forza - one header line added to every HTTP request the game sends to its title servers.
//
// The Forza Live server takes a title service call's player from the X-UserId header the
// game writes, which anyone can write. Our game adds its Live identity's friends key
// (X-Friends-Key, the secret the server bound to the XUID at the first HELLO; the launcher's
// shader list upload proves itself the same way), so the server can tell the player's own
// calls from someone else's (server/forza_live_lsp.py verify_caller). The game's requests
// are HTTP/1.1 with Content-Length over one kept-alive connection (docs/title_services.md);
// the head of each request is held until it is complete, the line goes in before its blank
// line, and the body passes through. LspTitleStream keeps the socket's nonblocking contract
// on top of that (live_tcp.cpp). Header-only so tools/test_lsp_request_header.cpp can test it
// without the runtime.

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

// A title server connection's outgoing side with the line added, behaving like the
// nonblocking socket the game made (FIONBIO) and with exact progress: the bytes the line adds
// mean the request stream that goes out differs from the game's, so the stream keeps what
// the socket has not taken yet (Pending) and sends it before anything newer.
//
// Send(data, size, send) returns how many of the game's bytes were taken (all of them, once
// nothing older is waiting: they go through LspRequestHeader and whatever the socket does not
// take now waits), kWouldBlock when older output is still waiting (nothing taken, as a full
// socket would say), or kError after a hard socket error (the connection is dead; the error
// is the socket's). Taken bytes are never sent twice or dropped, and nothing here waits:
// Flush (from send, recv and select) moves waiting output on as the socket takes it.
//
// `send(data, size, would_block)` writes to the socket: bytes written, or -1 with
// would_block set when it is full.
class LspTitleStream {
 public:
  static constexpr int64_t kWouldBlock = -2;
  static constexpr int64_t kError = -1;

  explicit LspTitleStream(std::string line) : header_(std::move(line)) {}

  template <class SendFn>
  int64_t Send(const uint8_t* data, size_t size, SendFn&& send) {
    if (failed_) return kError;
    const int64_t flushed = Flush(send);
    if (flushed != 0) return flushed;  // older output waits (or the socket failed)
    out_ = header_.Feed(data, size);
    sent_ = 0;
    if (Flush(send) == kError) failed_ = true;  // reported by the next call
    return int64_t(size);
  }

  // Sends what is waiting: 0 when nothing is left, kWouldBlock when the socket is full, kError.
  template <class SendFn>
  int64_t Flush(SendFn&& send) {
    if (failed_) return kError;
    while (sent_ < out_.size()) {
      bool would_block = false;
      const int64_t n = send(out_.data() + sent_, out_.size() - sent_, would_block);
      if (n > 0) {
        sent_ += size_t(n);
        continue;
      }
      if (n < 0 && !would_block) {
        failed_ = true;
        return kError;
      }
      return kWouldBlock;
    }
    out_.clear();
    sent_ = 0;
    return 0;
  }

  // Bytes taken from the game that the socket has not taken yet.
  size_t Pending() const { return out_.size() - sent_; }
  // A request head held until it is complete (tests).
  size_t HeldHead() const { return header_.Pending(); }

 private:
  LspRequestHeader header_;
  std::string out_;
  size_t sent_ = 0;
  bool failed_ = false;
};

}  // namespace forza
