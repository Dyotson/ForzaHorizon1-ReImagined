// The on-screen keyboard (kernel/live_osk.h): the game's text prompts (kPrompt, #294) and
// the friends screen's keyboard (kFriends), which must keep working as before.
// g++ -std=c++20 -Wall -Wextra -Werror -I forza-recomp/src/kernel forza-recomp/tests/osk_test.cpp
#include "live_osk.h"

#include <cctype>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace forza;
using namespace forza::pad;

// Presses each button in turn; the result of the last.
static Osk::Result Press(Osk& osk, std::string& text, size_t max,
                         std::initializer_list<uint16_t> presses) {
  Osk::Result r = Osk::kTyping;
  for (uint16_t p : presses) r = osk.Press(p, text, max);
  return r;
}

static void PromptGrid() {
  Osk osk(Osk::kPrompt);
  std::string text;
  osk.Open(text);
  CHECK(osk.open && osk.Selected() == "a");
  Press(osk, text, 20, {kRight, kRight});
  CHECK(osk.Selected() == "c");
  Press(osk, text, 20, {kLeft, kLeft, kLeft});
  CHECK(osk.Selected() == "j");  // wraps
  Press(osk, text, 20, {kDown});
  CHECK(osk.Selected() == "t");
  Press(osk, text, 20, {kUp, kUp});
  CHECK(osk.row() == 4 && osk.Selected() == "Done");  // wraps onto the function row
  // The function row: six keys over the ten columns, stepped one key at a time.
  Press(osk, text, 20, {kRight});
  CHECK(osk.Selected() == "Shift");
  const char* keys[] = {"Symbols", "Space", "Delete", "Cancel", "Done", "Shift"};
  for (const char* k : keys) {
    Press(osk, text, 20, {kRight});
    CHECK(osk.Selected() == k);
  }
  Press(osk, text, 20, {kLeft});
  CHECK(osk.Selected() == "Done");
  Press(osk, text, 20, {kDown});
  CHECK(osk.row() == 0 && osk.Selected() == "j");
  // Y: abc, ABC, symbols, abc. The symbols page has three rows; the cursor stays on a row.
  Press(osk, text, 20, {kY});
  CHECK(osk.page() == 1 && osk.Selected() == "J");
  Press(osk, text, 20, {kUp, kUp});  // the digits
  CHECK(osk.row() == 3 && osk.Selected() == "0");
  Press(osk, text, 20, {kY});
  CHECK(osk.page() == 2 && osk.row() == 2 && osk.Selected() == "0");
  Press(osk, text, 20, {kDown});
  CHECK(osk.row() == 3 && osk.Selected() == "Done");  // the function row on this page
  Press(osk, text, 20, {kY});
  CHECK(osk.page() == 0 && osk.row() == 4 && osk.Selected() == "Done");
  // Shift and Symbols keys.
  Press(osk, text, 20, {kLeft, kLeft, kLeft, kLeft, kLeft});
  CHECK(osk.Selected() == "Shift");
  Press(osk, text, 20, {kA});
  CHECK(osk.page() == 1 && osk.Selected() == "abc");
  Press(osk, text, 20, {kRight, kA});
  CHECK(osk.page() == 2 && osk.row() == 3);
  Press(osk, text, 20, {kA});
  CHECK(osk.page() == 0);
  CHECK(text.empty());
  // Render: the cursor's key in brackets.
  const std::string r = osk.Render();
  CHECK(r.find("[abc]") != std::string::npos || r.find("[Symbols]") != std::string::npos);
}

static void PromptEditing() {
  Osk osk(Osk::kPrompt);
  std::string text = "Photo";
  osk.Open(text);
  CHECK(osk.cursor() == 5 && osk.Field(text) == "Photo|");
  // A types the key at the cursor; X a space.
  Press(osk, text, 20, {kX, kDown, kDown, kDown, kRight, kA});  // '2'
  CHECK(text == "Photo 2");
  // LB/RB move the cursor; typing goes there.
  Press(osk, text, 20, {kLB, kLB, kUp, kUp, kUp, kLeft, kA});  // 'a' before " 2"
  CHECK(text == "Photoa 2" && osk.Field(text) == "Photoa| 2");
  // B deletes before the cursor.
  Press(osk, text, 20, {kB});
  CHECK(text == "Photo 2" && osk.cursor() == 5);
  Press(osk, text, 20, {kRB, kRB, kRB});  // stops at the end
  CHECK(osk.cursor() == 7);
  // B at the start of a text does nothing; it does not cancel.
  for (int i = 0; i < 9; ++i) osk.Press(kLB, text, 20);
  CHECK(osk.cursor() == 0 && osk.Press(kB, text, 20) == Osk::kTyping && text == "Photo 2");
  // The Delete key deletes too; Y then A types upper case.
  Press(osk, text, 20, {kRB, kRB, kUp, kLeft, kLeft, kLeft});
  CHECK(osk.Selected() == "Delete");
  Press(osk, text, 20, {kA});
  CHECK(text == "Poto 2" && osk.cursor() == 1);
  Press(osk, text, 20, {kY, kDown, kRight, kRight});
  CHECK(osk.Selected() == "H");
  osk.Press(kA, text, 20);
  CHECK(text == "PHoto 2");
  // UTF-8 from the game's default text moves and deletes by character.
  std::string accent = "Citro\xC3\xABn";
  osk.Open(accent);
  osk.Press(kLB, accent, 20);
  osk.Press(kB, accent, 20);
  CHECK(accent == "Citron");
}

static void PromptLimitAndFilter() {
  Osk osk(Osk::kPrompt);
  std::string text;
  osk.Open(text);
  for (int i = 0; i < 10; ++i) osk.Press(kA, text, 4);
  CHECK(text == "aaaa");
  osk.Press(kX, text, 4);
  CHECK(text == "aaaa");
  // The keyboard: only what the keys have, within the limit, at the cursor.
  text.clear();
  osk.Open(text);
  osk.Type("My: tune?/\"<>|*\\\xC3\xA9!", text, 15);
  CHECK(text == "My tune!");
  osk.MoveCursor(-1, text);
  osk.Type("s", text, 15);
  CHECK(text == "My tunes!");
  osk.Type("0123456789", text, 15);
  CHECK(text == "My tunes012345!" && Utf16Length(text) == 15);
  // A length in UTF-16 characters: an accented default counts one.
  std::string accent = "\xC3\xA9";
  osk.Open(accent);
  osk.Type("abc", accent, 3);
  CHECK(accent == "\xC3\xA9" "ab");
  // Every key of every page is a character a prompt allows.
  for (char c = 32; c < 127; ++c) {
    const bool want = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' ||
                      std::string("!@#$%&'()+=-_.,;~[]^").find(c) != std::string::npos;
    CHECK(Osk::Allowed(c) == want);
  }
  Osk all(Osk::kPrompt);
  std::string typed;
  all.Open(typed);
  for (int page = 0; page < 3; ++page) {
    for (int row = 0; row < (page == 2 ? 3 : 4); ++row) {
      for (int k = 0; k < 10; ++k) {
        std::string one;
        all.Press(kA, one, 1);
        CHECK(one.size() == 1 && Osk::Allowed(one[0]));
        all.Press(kRight, one, 1);
      }
      std::string none;
      all.Press(kDown, none, 1);
    }
    std::string none;
    all.Press(kDown | kY, none, 1);
  }
}

static void PromptConfirmCancel() {
  Osk osk(Osk::kPrompt);
  std::string text = "x";
  osk.Open(text);
  CHECK(osk.Press(kStart, text, 10) == Osk::kDone && !osk.open && text == "x");
  osk.Open(text);
  CHECK(osk.Press(kB, text, 10) == Osk::kTyping && text.empty());
  CHECK(osk.Press(kB, text, 10) == Osk::kClosed && !osk.open);  // B on an empty text
  text = "abc";
  osk.Open(text);
  Press(osk, text, 10, {kUp, kLeft, kLeft});
  CHECK(osk.Selected() == "Cancel" && osk.Press(kA, text, 10) == Osk::kClosed && !osk.open);
  osk.Open(text);
  Press(osk, text, 10, {kUp, kLeft});
  CHECK(osk.Selected() == "Done" && osk.Press(kA, text, 10) == Osk::kDone && text == "abc");
}

// The friends screen's keyboard, as before: X deletes, Y space, B closes, Start done; five
// function keys; typing at the end; a byte limit.
static void Friends() {
  Osk osk;
  std::string text = "ab";
  osk.Open();
  CHECK(osk.Selected() == "a");
  osk.Press(kA, text, 10);
  CHECK(text == "aba");
  osk.Press(kX, text, 10);
  osk.Press(kY, text, 10);
  CHECK(text == "ab ");
  Press(osk, text, 10, {kUp});
  CHECK(osk.row() == 4 && osk.Selected() == "Shift");
  const char* keys[] = {"Symbols", "Space", "Delete", "Done", "Shift"};
  for (const char* k : keys) {
    Press(osk, text, 10, {kRight});
    CHECK(osk.Selected() == k);
  }
  osk.Press(kA, text, 10);  // Shift
  Press(osk, text, 10, {kDown});
  osk.Press(kA, text, 10);
  CHECK(text == "ab A");
  Press(osk, text, 10, {kUp, kRight, kA, kDown, kDown, kA});  // Symbols, then '/'
  CHECK(text == "ab A/");
  CHECK(osk.Press(kLB | kRB, text, 10) == Osk::kTyping && text == "ab A/");  // no text cursor
  CHECK(osk.Press(kB, text, 10) == Osk::kClosed && !osk.open && text == "ab A/");
  osk.Open();
  CHECK(osk.Press(kStart, text, 10) == Osk::kDone);
  std::string full(10, 'x');
  osk.Open();
  osk.Press(kA, full, 10);
  CHECK(full.size() == 10);
  std::string utf = "a\xC3\xA9";
  osk.Open();
  osk.Press(kX, utf, 10);
  CHECK(utf == "a");
}

// The pad reader: a press counts on release, not while a keyboard key was down.
static void Reader() {
  PadReader r;
  r.Reset();
  CHECK(r.Poll(kA, 0, 0, 0) == 0);  // held when the prompt opened
  CHECK(r.Poll(0, 0, 0, 0) == 0);
  CHECK(r.Poll(kStart, 0, 0, 0) == 0);
  CHECK(r.Poll(0, 0, 0, 0) == kStart);
  CHECK(r.Poll(kA, 0, 0, 1) == 0);
  CHECK(r.Poll(0, 0, 0, 2) == 0);  // a key went down meanwhile (mnk_mode)
  CHECK(r.Poll(0, 0, -30000, 2) == 0);
  CHECK(r.Poll(0, 0, 0, 2) == kDown);
}

int main() {
  PromptGrid();
  PromptEditing();
  PromptLimitAndFilter();
  PromptConfirmCancel();
  Friends();
  Reader();
  std::puts(failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
