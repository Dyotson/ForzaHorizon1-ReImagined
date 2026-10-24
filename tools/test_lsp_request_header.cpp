// Standalone: c++ -std=c++17 tools/test_lsp_request_header.cpp -o test_lsp_request_header
// The friends key line added to the game's title server requests, and the nonblocking stream
// that sends them (forza-recomp/src/kernel/lsp_request_header.h).
#include "../forza-recomp/src/kernel/lsp_request_header.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace {
const std::string kLine = "X-Friends-Key: 000102030405060708090A0B0C0D0E0F\r\n";

std::string Feed(forza::LspRequestHeader& h, const std::string& s) {
  return h.Feed(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string Request(const std::string& body, const std::string& extra = "") {
  return "POST /Services.Horizon/o.xtsw HTTP/1.1\r\nHost: forza\r\nContent-Type: bin/xtsw\r\n"
         "Content-Length: " + std::to_string(body.size()) + "\r\nX-UserId: 0x9000000001241\r\n" +
         extra + "\r\n" + body;
}

std::string Expected(const std::string& body) {
  std::string r = Request(body);
  r.insert(r.find("\r\n\r\n") + 2, kLine);
  return r;
}

// A socket with `room` bytes of buffer: send takes what fits and says "would block" when full.
struct FakeSocket {
  size_t room = 1000;
  size_t used = 0;
  std::string wire;   // what the peer has received
  bool broken = false;
  int64_t operator()(const char* data, size_t size, bool& would_block) {
    if (broken) return -1;
    if (used >= room) {
      would_block = true;
      return -1;
    }
    const size_t n = std::min(size, room - used);
    wire.append(data, n);
    used += n;
    return int64_t(n);
  }
  void Drain() { used = 0; }  // the peer read everything
};

// The game's send loop: chunks of `chunk` bytes, a WSAEWOULDBLOCK means try the same bytes
// again once the socket has room (the peer reads, `on_block`).
template <class Send, class OnBlock>
void GuestSend(forza::LspTitleStream& stream, Send& send, const std::string& data, size_t chunk,
               OnBlock on_block, size_t* max_pending = nullptr) {
  size_t at = 0;
  while (at < data.size()) {
    const size_t n = std::min(chunk, data.size() - at);
    const auto t0 = std::chrono::steady_clock::now();
    const int64_t took = stream.Send(reinterpret_cast<const uint8_t*>(data.data()) + at, n, send);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50));  // never waits
    if (max_pending) *max_pending = std::max(*max_pending, stream.Pending());
    if (took == forza::LspTitleStream::kWouldBlock) {
      on_block();
      continue;
    }
    CHECK(took == int64_t(n));  // all or nothing
    at += n;
  }
}
}  // namespace

int main() {
  {  // one call: head and body together, then a second request on the kept-alive connection
    forza::LspRequestHeader h(kLine);
    const std::string body("\0\1\r\n\r\nbody bytes that look like a head", 39);
    CHECK(Feed(h, Request(body)) == Expected(body));
    CHECK(Feed(h, Request("x")) == Expected("x"));
    CHECK(h.Pending() == 0);
  }
  {  // byte by byte: nothing goes out before the head is complete
    forza::LspRequestHeader h(kLine);
    const std::string req = Request("abc");
    const size_t head = req.find("\r\n\r\n") + 4;
    std::string out;
    for (size_t i = 0; i < req.size(); ++i) {
      out += Feed(h, req.substr(i, 1));
      if (i + 1 < head) CHECK(out.empty());
    }
    CHECK(out == Expected("abc"));
  }
  {  // the head split across calls, body and the next head in one buffer
    forza::LspRequestHeader h(kLine);
    const std::string two = Request("12345") + Request("");
    std::string out = Feed(h, two.substr(0, 30));
    CHECK(out.empty() && h.Pending() == 30);
    out += Feed(h, two.substr(30));
    CHECK(out == Expected("12345") + Expected(""));
  }
  {  // a request that already has the header keeps its own
    forza::LspRequestHeader h(kLine);
    const std::string req = Request("z", "x-friends-key: FFFF\r\n");
    CHECK(Feed(h, req) == req);
  }
  {  // no Content-Length: a request without a body
    forza::LspRequestHeader h(kLine);
    const std::string get = "GET /status HTTP/1.1\r\nHost: forza\r\n\r\n";
    std::string want = get;
    want.insert(want.size() - 2, kLine);
    CHECK(Feed(h, get + get) == want + want);
  }
  {  // not HTTP: a head that never ends is passed through after kMaxHead bytes
    forza::LspRequestHeader h(kLine);
    const std::string junk(forza::LspRequestHeader::kMaxHead + 10, 'j');
    CHECK(Feed(h, junk) == junk);
    CHECK(Feed(h, "more\r\n\r\n") == "more\r\n\r\n");
  }
  {  // a full socket: the game's bytes are taken all or none, the stream goes out once
    forza::LspTitleStream stream(kLine);
    FakeSocket sock;
    std::string body(200000, '\0');
    for (size_t i = 0; i < body.size(); ++i) body[i] = char(i * 7);
    const std::string two = Request(body) + Request("tail");
    size_t blocked = 0, max_pending = 0;
    GuestSend(stream, sock, two, 4096, [&] { ++blocked; sock.Drain(); }, &max_pending);
    while (stream.Pending()) {
      CHECK(stream.Flush(sock) == forza::LspTitleStream::kWouldBlock);
      sock.Drain();
      if (stream.Flush(sock) == 0) break;
    }
    CHECK(sock.wire == Expected(body) + Expected("tail"));
    CHECK(blocked > 100);
    CHECK(max_pending <= 4096 + kLine.size());  // one send's worth, no more
  }
  {  // a socket that fails after taking bytes: those are not taken twice, the error follows
    forza::LspTitleStream stream(kLine);
    FakeSocket sock;
    sock.room = 10;
    const std::string req = Request("abc");
    CHECK(stream.Send(reinterpret_cast<const uint8_t*>(req.data()), req.size(), sock) == int64_t(req.size()));
    sock.broken = true;
    CHECK(stream.Flush(sock) == forza::LspTitleStream::kError);
    CHECK(stream.Send(reinterpret_cast<const uint8_t*>(req.data()), req.size(), sock) == forza::LspTitleStream::kError);
    CHECK(sock.wire == Expected("abc").substr(0, 10));
  }
#ifndef _WIN32
  {  // a real nonblocking socket whose peer stops reading: no call waits, nothing is lost or
     // sent twice, the line goes in once per request
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int small = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    CHECK(fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK) == 0);
    CHECK(fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK) == 0);
    auto send = [&](const char* data, size_t size, bool& would_block) -> int64_t {
      const ssize_t n = ::send(sv[0], data, size, MSG_NOSIGNAL);
      if (n < 0) would_block = errno == EAGAIN || errno == EWOULDBLOCK;
      return n;
    };
    std::string received;
    auto read_some = [&] {
      char buf[65536];
      ssize_t n;
      while ((n = ::recv(sv[1], buf, sizeof(buf), 0)) > 0) received.append(buf, size_t(n));
    };
    forza::LspTitleStream stream(kLine);
    std::string body(1 << 20, 'b');
    const std::string req = Request(body) + Request("second");
    // the peer reads nothing at first: the stream fills the socket, then says "would block"
    int64_t took = 0;
    size_t at = 0, blocked = 0;
    while (blocked == 0) {
      const auto t0 = std::chrono::steady_clock::now();
      took = stream.Send(reinterpret_cast<const uint8_t*>(req.data()) + at, 4096, send);
      CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50));
      if (took == forza::LspTitleStream::kWouldBlock) ++blocked;
      else at += size_t(took);
    }
    // then it reads as the rest goes out
    GuestSend(stream, send, req.substr(at), 4096, read_some);
    while (stream.Pending()) {
      stream.Flush(send);
      read_some();
    }
    read_some();
    CHECK(received == Expected(body) + Expected("second"));
    close(sv[0]);
    close(sv[1]);
  }
#endif
  std::puts("lsp_request_header: OK");
  return 0;
}
