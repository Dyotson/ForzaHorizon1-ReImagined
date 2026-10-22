// Gamertags from the network are cleaned before the game sees them (kernel/gamertag_text.h).
// g++ -std=c++20 -I forza-recomp/src/kernel forza-recomp/tests/gamertag_text_test.cpp
#include "gamertag_text.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
  using forza::SanitizeGamertag;
  CHECK(SanitizeGamertag("Dyotson") == "Dyotson");
  CHECK(SanitizeGamertag("  Two   Words ") == "Two Words");
  CHECK(SanitizeGamertag("A\xCC\x81\xCC\x82" "bc") == "Abc");             // combining marks
  CHECK(SanitizeGamertag("\xE4\xB8\xAD\xE6\x96\x87") == "");               // CJK
  CHECK(SanitizeGamertag("a\xE2\x80\xAE" "b") == "ab");                    // U+202E
  CHECK(SanitizeGamertag(std::string("a\0b\n", 4)) == "a b");
  CHECK(SanitizeGamertag(std::string(40, 'x')) == std::string(15, 'x'));
  CHECK(SanitizeGamertag("Abcdefghijklmn Pqr") == "Abcdefghijklmn");
  CHECK(SanitizeGamertag("") == "");

  uint8_t buf[16];
  std::memset(buf, 0xAA, sizeof buf);
  forza::StoreGamertag(buf, sizeof buf, std::string(30, 'y'));
  CHECK(buf[14] == 'y' && buf[15] == 0);                                  // 15 characters + NUL
  forza::StoreGamertag(buf, sizeof buf, "\xE4\xB8\xAD");
  CHECK(buf[0] == 0);
  uint8_t small[4];
  forza::StoreGamertag(small, sizeof small, "abcdefgh");
  CHECK(small[3] == 0 && small[2] == 'c');
  forza::StoreGamertag(nullptr, 16, "x");
  std::puts(failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
