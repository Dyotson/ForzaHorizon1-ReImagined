// Standalone: c++ -std=c++17 tools/test_lsp_request_header.cpp -o test_lsp_request_header
// The friends key line added to the game's title server requests
// (forza-recomp/src/kernel/lsp_request_header.h).
#include "../forza-recomp/src/kernel/lsp_request_header.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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
  std::puts("lsp_request_header: OK");
  return 0;
}
