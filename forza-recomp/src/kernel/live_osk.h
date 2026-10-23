// forza - controller helpers for the text screens over the game (live_friends.cpp; the
// invite picker in live_invites.cpp works the same way) and the game's text prompts
// (xam_keyboard.cpp): a pad reader that counts a press only when it is released with no
// keyboard key down meanwhile (with mnk_mode the keyboard also drives the pad, so typing
// "sw" must not move the cursor), and an on-screen keyboard for typing with a controller.

#pragma once

#include "../platform/hud_overlay.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

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

// The UTF-8 character boundary after / before byte `i` of `s`.
inline size_t Utf8Next(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  for (++i; i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80;) ++i;
  return i;
}
inline size_t Utf8Prev(const std::string& s, size_t i) {
  if (i == 0) return 0;
  for (--i; i > 0 && (uint8_t(s[i]) & 0xC0) == 0x80;) --i;
  return i;
}

// The UTF-16 code units of a UTF-8 text (what a game's text buffer holds).
inline size_t Utf16Length(std::string_view s) {
  size_t n = 0;
  for (const char c : s) {
    if ((uint8_t(c) & 0xC0) != 0x80) n += uint8_t(c) >= 0xF0 ? 2 : 1;
  }
  return n;
}

// The on-screen keyboard: a page of character rows (lower case, upper case, symbols) over a
// row of function keys. Each row's keys share ten columns, so moving up or down stays
// about where it was. Two layouts:
// - kFriends (the friends screen's search and chat): D-pad or left stick moves, A presses
//   the key, X deletes, Y types a space, B or Start closes it; Shift, Symbols, Space,
//   Delete, Done. Text goes at the end, at most `max` bytes.
// - kPrompt (the game's text prompts, xam_keyboard.cpp), as on the 360: A presses the key,
//   B deletes before the text cursor (on an empty text it cancels), X types a space, Y
//   switches abc, ABC and symbols, LB/RB move the text cursor, Start confirms; Shift,
//   Symbols, Space, Delete, Cancel, Done. At most `max` UTF-16 characters, and only
//   letters, digits, space and kPromptPunctuation: ASCII, which the server keeps, and
//   none that a Windows file name refuses (a photo's folder is named after it, #211).
class Osk {
 public:
  enum Layout { kFriends, kPrompt };
  static constexpr const char* kPromptPunctuation = "!@#$%&'()+=-_.,;~[]^";

  explicit Osk(Layout layout = kFriends) : layout_(layout) {}

  bool open = false;

  // The cursor on the first key (the page stays).
  void Open() {
    open = true;
    row_ = col_ = 0;
  }
  // A prompt: the first page too, and the text cursor after `text`.
  void Open(const std::string& text) {
    Open();
    page_ = 0;
    cursor_ = text.size();
  }

  enum Result { kTyping, kClosed, kDone };
  // One poll's presses (PadReader); edits `text`. kClosed: closed (B on the friends
  // screen) or cancelled (a prompt); kDone: Start or the Done key.
  Result Press(uint16_t presses, std::string& text, size_t max) {
    using namespace pad;
    Clamp(text);
    const int rows = CharRows() + 1;
    if (presses & kUp) row_ = (row_ + rows - 1) % rows;
    if (presses & kDown) row_ = (row_ + 1) % rows;
    if (presses & kLeft) col_ = Step(-1);
    if (presses & kRight) col_ = Step(1);
    if (layout_ == kFriends) {
      if (presses & kX) Delete(text);
      if (presses & kY) Insert(" ", text, max);
      if (presses & (kB | kStart)) {
        open = false;
        return presses & kStart ? kDone : kClosed;
      }
    } else {
      if (presses & kLB) MoveCursor(-1, text);
      if (presses & kRB) MoveCursor(1, text);
      if (presses & kX) Insert(" ", text, max);
      if (presses & kY) SetPage((page_ + 1) % 3);
      if (presses & kStart) {
        open = false;
        return kDone;
      }
      if (presses & kB) {
        if (text.empty()) {
          open = false;
          return kClosed;
        }
        Delete(text);
      }
    }
    if (presses & kA) {
      if (row_ < CharRows()) {
        Insert(std::string(1, Rows()[row_][Key()]), text, max);
      } else {
        switch (Functions()[Key()]) {
          case kFnShift: SetPage(page_ == 1 ? 0 : 1); break;
          case kFnSymbols: SetPage(page_ == 2 ? 0 : 2); break;
          case kFnSpace: Insert(" ", text, max); break;
          case kFnDelete: Delete(text); break;
          case kFnCancel: open = false; return kClosed;
          case kFnDone: open = false; return kDone;
        }
      }
    }
    return kTyping;
  }

  // The physical keyboard's text, typed at the text cursor (a prompt drops what its keys
  // don't have).
  void Type(std::string_view typed, std::string& text, size_t max) {
    Clamp(text);
    for (size_t i = 0; i < typed.size();) {
      size_t n = 1;
      while (i + n < typed.size() && (uint8_t(typed[i + n]) & 0xC0) == 0x80) ++n;
      if (layout_ == kFriends || (n == 1 && Allowed(typed[i]))) {
        Insert(std::string(typed.substr(i, n)), text, max);
      }
      i += n;
    }
  }
  // Deletes the character before the text cursor.
  void Delete(std::string& text) {
    Clamp(text);
    const size_t from = Utf8Prev(text, cursor_);
    text.erase(from, cursor_ - from);
    cursor_ = from;
  }
  // Moves the text cursor one character back (by < 0) or on.
  void MoveCursor(int by, const std::string& text) {
    Clamp(text);
    cursor_ = by < 0 ? Utf8Prev(text, cursor_) : by > 0 ? Utf8Next(text, cursor_) : cursor_;
  }

  // A prompt's characters: letters, digits, space and kPromptPunctuation.
  static bool Allowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ' ' || (c && std::strchr(kPromptPunctuation, c));
  }

  // The keyboard, one line per row (the cursor's row highlighted, its key in brackets).
  std::string Render() const {
    std::string out;
    for (int r = 0; r < CharRows(); ++r) {
      std::string line = r == row_ ? std::string(1, kHudHighlight) : "";
      const char* keys = Rows()[r];
      for (int c = 0; keys[c]; ++c) {
        const char k = keys[c];
        line += r == row_ && c == Key() ? std::string("[") + k + "]" : std::string(" ") + k + " ";
      }
      out += line + "\n";
    }
    const bool fn_row = row_ == CharRows();
    std::string line = fn_row ? std::string(1, kHudHighlight) : "";
    for (int k = 0; k < FunctionCount(); ++k) {
      const char* label = Label(Functions()[k]);
      line += fn_row && Key() == k ? std::string("[") + label + "] "
                                   : std::string(" ") + label + "  ";
    }
    return out + line + "\n";
  }

  // `text` with the text cursor shown ("ab|c").
  std::string Field(const std::string& text) const {
    const size_t at = layout_ == kFriends ? text.size() : std::min(cursor_, text.size());
    return text.substr(0, at) + "|" + text.substr(at);
  }

  // The key under the cursor ("a", "Done"), for logs.
  std::string Selected() const {
    return row_ < CharRows() ? std::string(1, Rows()[row_][Key()]) : Label(Functions()[Key()]);
  }

  int row() const { return row_; }
  int key() const { return Key(); }
  int page() const { return page_; }
  size_t cursor() const { return cursor_; }

  static constexpr const char* kHint =
      "D-pad: move   A: key   X: delete   Y: space   B or Start: done";
  static constexpr const char* kSendHint =
      "D-pad: move   A: key   X: delete   Y: space   Start: send   B: close";
  static constexpr const char* kPromptHint =
      "D-pad: move   A: key   B: delete   X: space   Y: ABC / symbols\n"
      "LB RB: move in the text   Start: done   B on an empty text: cancel";

 private:
  enum Function { kFnShift, kFnSymbols, kFnSpace, kFnDelete, kFnCancel, kFnDone };

  const char* const* Rows() const {
    static const char* const kLower[4] = {"abcdefghij", "klmnopqrst", "uvwxyz-_.,", "1234567890"};
    static const char* const kUpper[4] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_.,", "1234567890"};
    static const char* const kSymbolRows[4] = {"!?@#$%&*()", "+=/:;'\"<>~", "[]{}^|\\`.,",
                                               "1234567890"};
    static const char* const kPromptSymbols[3] = {"!@#$%&'()+", "=-_.,;~[]^", "1234567890"};
    if (page_ == 1) return kUpper;
    if (page_ == 2) return layout_ == kPrompt ? kPromptSymbols : kSymbolRows;
    return kLower;
  }
  int CharRows() const { return page_ == 2 && layout_ == kPrompt ? 3 : 4; }

  const Function* Functions() const {
    static const Function kFriendKeys[5] = {kFnShift, kFnSymbols, kFnSpace, kFnDelete, kFnDone};
    static const Function kPromptKeys[6] = {kFnShift,  kFnSymbols, kFnSpace,
                                            kFnDelete, kFnCancel,  kFnDone};
    return layout_ == kPrompt ? kPromptKeys : kFriendKeys;
  }
  int FunctionCount() const { return layout_ == kPrompt ? 6 : 5; }
  const char* Label(Function f) const {
    switch (f) {
      case kFnShift: return page_ == 1 ? "abc" : "Shift";
      case kFnSymbols: return page_ == 2 ? "abc" : "Symbols";
      case kFnSpace: return "Space";
      case kFnDelete: return "Delete";
      case kFnCancel: return "Cancel";
      default: return "Done";
    }
  }

  // The cursor's row has this many keys, spread over the ten columns.
  int RowKeys() const {
    return row_ < CharRows() ? int(std::strlen(Rows()[row_])) : FunctionCount();
  }
  int Key() const { return col_ * RowKeys() / 10; }
  // The column of the key `by` keys on (wrapping): the first column of that key.
  int Step(int by) const {
    const int n = RowKeys(), k = ((Key() + by) % n + n) % n;
    return (k * 10 + n - 1) / n;
  }
  // Another page; the cursor stays on the function row, or on the last character row.
  void SetPage(int page) {
    const bool fn_row = row_ == CharRows();
    page_ = page;
    row_ = fn_row ? CharRows() : std::min(row_, CharRows() - 1);
  }

  size_t Length(std::string_view text) const {
    return layout_ == kFriends ? text.size() : Utf16Length(text);
  }
  void Insert(const std::string& s, std::string& text, size_t max) {
    if (Length(text) + Length(s) > max) return;
    text.insert(cursor_, s);
    cursor_ += s.size();
  }
  // The friends screen types at the end; a prompt's cursor stays on a character boundary.
  void Clamp(const std::string& text) {
    if (layout_ == kFriends || cursor_ > text.size()) cursor_ = text.size();
    while (cursor_ > 0 && cursor_ < text.size() && (uint8_t(text[cursor_]) & 0xC0) == 0x80) {
      --cursor_;
    }
  }

  Layout layout_;
  int row_ = 0, col_ = 0, page_ = 0;
  size_t cursor_ = 0;
};

}  // namespace forza
