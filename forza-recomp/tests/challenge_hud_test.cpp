// Standalone: no SDK or game data. Compile with C++17 and thread support.
#include "../src/platform/challenge_hud.h"
#include <cassert>
#include <limits>
#include <thread>

int main() {
  using namespace forza::challenge_hud;
  Mailbox hud;
  assert(!hud.Read(100));
  hud.Show("HEAD-TO-HEAD", "Driver challenges you\nX accept    B decline    (10)", 2, 100);
  auto prompt = *hud.Read(101);
  assert(prompt.first == "Driver challenges you");
  assert(prompt.second == "X accept    B decline    (10)");
  assert(NativeLines(prompt).headline == prompt.first);
  assert(NativeLines(prompt).caption == prompt.second);
  assert(hud.Read(2099) && !hud.Read(2100));
  hud.Show(prompt.title, prompt.first + "\n" + prompt.second, 2, 1000);
  assert(hud.Read(2500)->revision == prompt.revision);
  hud.Show(prompt.title, "Driver challenges you\nX accept    B decline    (9)", 2, 1200);
  assert(hud.Read(1200)->revision != prompt.revision);
  hud.Show("HEAD-TO-HEAD", "vs Driver: race to the Wind Farm\n2.2 km, ahead, left", 3, 1300);
  assert(hud.Read(1300)->second == "2.2 km, ahead, left");
  assert(NativeLines(*hud.Read(1300)).caption == "2.2 km, ahead, left");
  hud.Show("YOU WIN", "You beat Driver", 6, 1400);
  assert(hud.Read(1400)->title == "YOU WIN");
  assert(hud.Read(1400)->second.empty());
  assert(NativeLines(*hud.Read(1400)).caption == "YOU WIN");
  hud.Clear();
  assert(!hud.Read(1401));
  hud.Show("Next", "one\ntwo\nthree", 1, 1500);
  assert(hud.Read(1500)->second == "two three");
  hud.Show("Next", "body", std::numeric_limits<double>::quiet_NaN(), 1501);
  assert(!hud.Read(1501));
  hud.Show("Next", "body", -1, 1501);
  assert(!hud.Read(1501));
  hud.Show("Next", "body", 1e30, 1501);
  assert(hud.Read(61500) && !hud.Read(61501));
  assert(Text("Driver") == u"Driver");
  assert(Text("Jos\xC3\xA9") == u"Jos\u00E9");
  assert(Text("\xF0\x9F\x8F\x81") == u"\U0001F3C1");
  assert(Text("\xF0\x9F\x8F\x81", 1).empty());
  assert(Text("\xC0\xAF") == u"\uFFFD\uFFFD");
  assert(Text("\xED\xA0\x80") == u"\uFFFD");
  assert(Text("\xC3") == u"\uFFFD");
  assert(Text(std::string("a\0b\nc", 5)) == u"a b c");
  assert(Text(std::string(1000, 'a')).size() == 120);
  // Concurrent publication must never return text from two different messages.
  std::thread network([&] {
    for (int i = 0; i < 10000; ++i)
      hud.Show(std::to_string(i), std::to_string(i) + "\n" + std::to_string(i), 1, 100);
  });
  for (int i = 0; i < 10000; ++i) {
    auto value = hud.Read(200);
    if (value && value->title != "Next") {
      assert(value->title == value->first);
      assert(value->first == value->second);
    }
  }
  network.join();
}
