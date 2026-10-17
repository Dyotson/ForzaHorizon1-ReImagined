// forza - controller helpers for the text screens over the game (live_friends.cpp; the
// invite picker in live_invites.cpp works the same way): a pad reader that counts a press
// only when it is released with no keyboard key down meanwhile (with mnk_mode the
// keyboard also drives the pad, so typing "sw" must not move the cursor), and an
// on-screen keyboard for typing with a controller.

#pragma once

#include "../platform/hud_overlay.h"

#include <cstdint>
#include <string>

namespace forza {

namespace pad {
constexpr uint16_t kUp = 0x1, kDown = 0x2, kLeft = 0x4, kRight = 0x8, kStart = 0x10,
                   kBack = 0x20, kLS = 0x40, kRS = 0x80, kLB = 0x100, kRB = 0x200, kA = 0x1000,
                   kB = 0x2000, kX = 0x4000, kY = 0x8000;
constexpr int kStickThreshold = 16000;
}  // namespace pad

// Presses of player 1's pad: buttons pressed and released with no keyboard key down since
// they were pressed (several at once count together, when the last is released); the left
// stick pushed one way and back counts as that D-pad direction.
class PadReader {
 public:
  // What is held now (the press that opened the screen) is not a press.
  void Reset() {
    prev_ = 0xFFFF;
    down_ = 0;
    stick_ = 0;
  }

  uint16_t Poll(uint16_t buttons, int left_x, int left_y, unsigned key_frames) {
    uint16_t out = 0;
    const uint16_t pressed = buttons & ~prev_;
    if (pressed && !down_) down_frames_ = key_frames;
    down_ |= pressed;
    prev_ = buttons;
    if (!buttons && down_) {
      if (key_frames == down_frames_) out |= down_;
      down_ = 0;
    }
    using namespace pad;
    const uint16_t dir = left_y > kStickThreshold    ? kUp
                         : left_y < -kStickThreshold ? kDown
                         : left_x > kStickThreshold  ? kRight
                         : left_x < -kStickThreshold ? kLeft
                                                     : 0;
    if (dir && !stick_) stick_frames_ = key_frames;
    if (!dir && stick_ && key_frames == stick_frames_) out |= stick_;
    stick_ = dir;
    return out;
  }

 private:
  uint16_t prev_ = 0xFFFF, down_ = 0, stick_ = 0;
  unsigned down_frames_ = 0, stick_frames_ = 0;
};

// Drops the last UTF-8 character of `s`.
inline void PopUtf8(std::string& s) {
  if (s.empty()) return;
  size_t end = s.size() - 1;
  while (end > 0 && (uint8_t(s[end]) & 0xC0) == 0x80) --end;
  s.erase(end);
}

// The on-screen keyboard: four rows of ten characters (lower case, upper case, symbols)
// and a row of Shift, Symbols, Space, Delete and Done. D-pad or left stick moves, A presses
// the key, X deletes, Y types a space, B or Start closes it.
class Osk {
 public:
  bool open = false;

  void Open() {
    open = true;
    row_ = col_ = 0;
  }

  // One poll's presses (PadReader); edits `text` (at most `max` bytes). Returns true when
  // the keyboard closed.
  bool Press(uint16_t presses, std::string& text, size_t max) {
    using namespace pad;
    if (presses & kUp) row_ = (row_ + 4) % 5;
    if (presses & kDown) row_ = (row_ + 1) % 5;
    if (presses & kLeft) col_ = row_ == 4 ? (col_ / 2 * 2 + 8) % 10 : (col_ + 9) % 10;
    if (presses & kRight) col_ = row_ == 4 ? (col_ / 2 * 2 + 2) % 10 : (col_ + 1) % 10;
    if (presses & kX) PopUtf8(text);
    if ((presses & kY) && text.size() < max) text += ' ';
    if (presses & (kB | kStart)) {
      open = false;
      return true;
    }
    if (presses & kA) {
      if (row_ < 4) {
        if (text.size() < max) text += Rows()[row_][col_];
      } else {
        switch (col_ / 2) {
          case 0: page_ = page_ == 1 ? 0 : 1; break;  // Shift
          case 1: page_ = page_ == 2 ? 0 : 2; break;  // Symbols / letters
          case 2: if (text.size() < max) text += ' '; break;
          case 3: PopUtf8(text); break;
          default: open = false; return true;          // Done
        }
      }
    }
    return false;
  }

  // The keyboard, one line per row (the cursor's row highlighted, its key in brackets).
  std::string Render() const {
    std::string out;
    for (int r = 0; r < 4; ++r) {
      std::string line = r == row_ ? std::string(1, kHudHighlight) : "";
      for (int c = 0; c < 10; ++c) {
        const char k = Rows()[r][c];
        line += r == row_ && c == col_ ? std::string("[") + k + "]" : std::string(" ") + k + " ";
      }
      out += line + "\n";
    }
    const char* keys[5] = {page_ == 1 ? "abc" : "Shift", page_ == 2 ? "abc" : "Symbols", "Space",
                           "Delete", "Done"};
    std::string line = row_ == 4 ? std::string(1, kHudHighlight) : "";
    for (int k = 0; k < 5; ++k) {
      line += row_ == 4 && col_ / 2 == k ? std::string("[") + keys[k] + "] "
                                         : std::string(" ") + keys[k] + "  ";
    }
    return out + line + "\n";
  }

  static constexpr const char* kHint =
      "D-pad: move   A: key   X: delete   Y: space   B or Start: done";

 private:
  const char* const* Rows() const {
    static const char* const kLower[4] = {"abcdefghij", "klmnopqrst", "uvwxyz-_.,", "1234567890"};
    static const char* const kUpper[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_.,", "1234567890"};
    static const char* const kSymbols[4] = {"!?@#$%&*()", "+=/:;'\"<>~", "[]{}^|\\`.,",
                                            "1234567890"};
    return page_ == 1 ? kUpper : page_ == 2 ? kSymbols : kLower;
  }

  int row_ = 0, col_ = 0, page_ = 0;
};

}  // namespace forza
