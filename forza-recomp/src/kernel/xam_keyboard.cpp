// forza - the game's text prompts (#294): naming a photo, a tune or a livery calls
// XamShowKeyboardUI. The SDK answers with a text window that needs a keyboard; here it is
// an on-screen keyboard over the game that a controller can type on, as on the 360
// (live_osk.h, layout kPrompt), and that takes the keyboard as well.
//
// - It shows the game's title, description and default text, in the HUD overlay
//   (platform/hud_overlay.h). The overlay is drawn over the presented frame and is not in
//   tools/live_shot.sh screenshots, so every keystroke and the result are logged
//   ("Keyboard UI: ...").
// - Controller: D-pad or left stick moves over the keys, A types the key, B deletes (on an
//   empty text it cancels), X types a space, Y switches abc, ABC and symbols, LB/RB move
//   the text cursor, Start or the Done key confirms, the Cancel key cancels.
// - Keyboard: typing, Backspace, Left/Right move the text cursor, Enter confirms, Escape
//   cancels.
// - Only letters, digits, space and the punctuation on the keys are taken (Osk::Allowed:
//   ASCII, which the server keeps of a photo's title, and nothing a Windows file name
//   refuses: a photo's folder is named after it, #211), at most the game's buffer length.
// - The game reads a neutral pad meanwhile: the input hook (input_autopress.cpp) hands
//   player 1's pad to KeyboardUiPad. A press counts when it is released (PadReader), so the
//   press that confirms never reaches the game; what is still held when the prompt closes
//   (Enter by keyboard, which mnk_mode also presents as a pad button) stays hidden from the
//   game until it is released.
// - As with the SDK's dialogs, the prompt runs on the kernel's deferred-completion thread:
//   XN_SYS_UI goes true before and false after, XamIsUIActive is true meanwhile, and the
//   overlapped completes with the text, or with X_ERROR_CANCELLED as the extended error.
//   Without the HUD (headless), or for a call without an overlapped (the prompt would hold
//   the thread that reads the pad), the SDK answers.

#include "../platform/guest_memory.h"
#include "../platform/hud_overlay.h"
#include "../platform/sdk_import.h"
#include "live_osk.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

using rex::X_RESULT;

namespace forza {
namespace {

constexpr uint32_t kNotifySystemUi = 0x9;   // XN_SYS_UI
constexpr size_t kMaxGuestText = 1024;      // longest title or description read
constexpr size_t kWrap = 60;                // the description's line length
constexpr auto kPoll = std::chrono::milliseconds(15);
constexpr auto kRefresh = std::chrono::seconds(1);

std::mutex mutex;
bool showing = false;      // the prompt is up: player 1's pad goes to it
PadReader reader;
uint16_t presses = 0;      // pad presses the prompt has not handled yet
uint16_t held = 0;         // the pad's buttons at the last poll
uint16_t latched = 0;      // held when the prompt closed: hidden from the game until released
unsigned key_frames = 0;   // the HUD's last reading (PadReader)
std::atomic<bool> busy{false};       // from the call until XN_SYS_UI goes false
std::atomic<bool> ui_active{false};  // while the prompt shows (XamIsUIActive)

// A guest UTF-16 (big endian) string, as UTF-8.
std::string ReadWide(uint8_t* base, uint32_t address) {
  std::string out;
  if (!address) return out;
  const uint8_t* p = GuestPtr(base, address);
  for (size_t i = 0; i < kMaxGuestText; ++i) {
    uint32_t c = uint32_t(p[2 * i]) << 8 | p[2 * i + 1];
    if (!c) break;
    if (c >= 0xD800 && c < 0xDC00) {
      const uint32_t low = uint32_t(p[2 * i + 2]) << 8 | p[2 * i + 3];
      if (low < 0xDC00 || low >= 0xE000) continue;
      c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
      ++i;
    } else if (c >= 0xDC00 && c < 0xE000) {
      continue;
    }
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | c >> 6);
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | c >> 12);
      out += char(0x80 | (c >> 6 & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | c >> 18);
      out += char(0x80 | (c >> 12 & 0x3F));
      out += char(0x80 | (c >> 6 & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// The UTF-8 `text` into a guest UTF-16 buffer of `units` (big endian, NUL terminated).
void WriteWide(uint8_t* base, uint32_t address, uint32_t units, const std::string& text) {
  uint8_t* p = GuestPtr(base, address);
  uint32_t n = 0;
  auto put = [&](uint32_t u) {
    p[2 * n] = uint8_t(u >> 8);
    p[2 * n + 1] = uint8_t(u);
    ++n;
  };
  for (size_t i = 0; i < text.size();) {
    const uint8_t b = uint8_t(text[i]);
    const size_t len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    uint32_t c = len == 1 ? b : b & (0x3F >> (len - 1));
    for (size_t k = 1; k < len && i + k < text.size(); ++k) c = c << 6 | (text[i + k] & 0x3F);
    i += len;
    if (c >= 0x10000) {
      if (n + 2 >= units) break;
      put(0xD800 + ((c - 0x10000) >> 10));
      put(0xDC00 + ((c - 0x10000) & 0x3FF));
    } else {
      if (n + 1 >= units) break;
      put(c);
    }
  }
  put(0);
}

// `text` broken into lines of at most `width` characters, at spaces where it can.
std::string Wrap(const std::string& text, size_t width) {
  std::string out, line;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find_first_of(" \n", start);
    if (end == std::string::npos) end = text.size();
    const std::string word = text.substr(start, end - start);
    if (!line.empty() && Utf16Length(line) + 1 + Utf16Length(word) > width) {
      out += line + "\n";
      line.clear();
    }
    line += (line.empty() ? "" : " ") + word;
    if (end < text.size() && text[end] == '\n') {
      out += line + "\n";
      line.clear();
    }
    start = end + 1;
  }
  return out + (line.empty() ? "" : line + "\n");
}

std::string ButtonNames(uint16_t p) {
  static const struct {
    uint16_t mask;
    const char* name;
  } kNames[] = {{pad::kUp, "Up"},    {pad::kDown, "Down"}, {pad::kLeft, "Left"},
                {pad::kRight, "Right"}, {pad::kStart, "Start"}, {pad::kBack, "Back"},
                {pad::kLB, "LB"},    {pad::kRB, "RB"},     {pad::kA, "A"},
                {pad::kB, "B"},      {pad::kX, "X"},       {pad::kY, "Y"}};
  std::string out;
  for (const auto& n : kNames) {
    if (p & n.mask) out += (out.empty() ? "" : "+") + std::string(n.name);
  }
  return out.empty() ? "-" : out;
}

struct Prompt {
  std::string title, description, text;
  size_t max = 0;  // UTF-16 characters, without the NUL
};

void Show(const Prompt& prompt, const Osk& osk, const std::string& text) {
  std::string body;
  if (!prompt.description.empty()) body += Wrap(prompt.description, kWrap) + "\n";
  body += "  " + osk.Field(text) + "      " + std::to_string(Utf16Length(text)) + "/" +
          std::to_string(prompt.max) + "\n\n";
  body += osk.Render() + "\n" + Osk::kPromptHint;
  HudShow(prompt.title, body, 5.0, false);
}

// Shows the prompt until it is confirmed (true, `text` the result) or cancelled.
bool RunPrompt(const Prompt& prompt, std::string& text) {
  Osk osk(Osk::kPrompt);
  text = prompt.text;
  osk.Open(text);
  {
    std::lock_guard lock(mutex);
    reader.Reset();
    presses = 0;
    showing = true;
  }
  HudWantKeys(true);
  HudTakeKeys();
  ui_active = true;
  Show(prompt, osk, text);
  auto shown = std::chrono::steady_clock::now();
  Osk::Result result = Osk::kTyping;
  while (result == Osk::kTyping) {
    std::this_thread::sleep_for(kPoll);
    const HudKeys keys = HudTakeKeys();
    uint16_t p;
    {
      std::lock_guard lock(mutex);
      key_frames = keys.key_frames;
      p = presses;
      presses = 0;
    }
    bool changed = false;
    if (!keys.typed.empty() || keys.backspace || keys.left || keys.right || keys.enter ||
        keys.escape) {
      osk.Type(keys.typed, text, prompt.max);
      for (int i = 0; i < keys.backspace; ++i) osk.Delete(text);
      if (keys.left) osk.MoveCursor(-1, text);
      if (keys.right) osk.MoveCursor(1, text);
      if (keys.enter) result = Osk::kDone;
      else if (keys.escape) result = Osk::kClosed;
      REXLOG_INFO("Keyboard UI: keys \"{}\"{}{}{} -> \"{}\"", keys.typed,
                  keys.backspace ? " Backspace" : "", keys.enter ? " Enter" : "",
                  keys.escape ? " Escape" : "", osk.Field(text));
      changed = true;
    }
    if (result == Osk::kTyping && p) {
      const std::string key = osk.Selected();
      result = osk.Press(p, text, prompt.max);
      REXLOG_INFO("Keyboard UI: pad {} on [{}] -> \"{}\"", ButtonNames(p), key, osk.Field(text));
      changed = true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (result == Osk::kTyping && (changed || now - shown >= kRefresh)) {
      Show(prompt, osk, text);
      shown = now;
    }
  }
  {
    std::lock_guard lock(mutex);
    showing = false;
    latched = held;
  }
  HudWantKeys(false);
  HudClear();
  ui_active = false;
  if (result == Osk::kDone) {
    REXLOG_INFO("Keyboard UI: done \"{}\"", text);
  } else {
    REXLOG_INFO("Keyboard UI: cancelled");
  }
  return result == Osk::kDone;
}

}  // namespace

uint16_t KeyboardUiPad(uint16_t buttons, int left_x, int left_y, bool& block_all) {
  std::lock_guard lock(mutex);
  held = buttons;
  if (!showing) {
    block_all = false;
    latched &= buttons;
    return latched;
  }
  presses |= reader.Poll(buttons, left_x, left_y, key_frames);
  block_all = true;
  return 0xFFFF;
}

}  // namespace forza

using forza::GuestFunc;

// DWORD XamShowKeyboardUI(DWORD user_index, DWORD flags, LPCWSTR default_text, LPCWSTR title,
//                         LPCWSTR description, LPWSTR buffer, DWORD buffer_length,
//                         XOVERLAPPED* overlapped)
extern "C" REX_FUNC(__imp__XamShowKeyboardUI) {
  static GuestFunc sdk = forza::SdkImport("__imp__XamShowKeyboardUI");
  const uint32_t buffer = ctx.r8.u32, units = ctx.r9.u32, overlapped = ctx.r10.u32;
  auto* kernel = rex::system::kernel_state();
  if (!forza::HudReady() || !kernel || !buffer || !units || !overlapped ||
      forza::busy.exchange(true)) {
    if (sdk) {
      sdk(ctx, base);
    } else {
      ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    }
    return;
  }
  forza::Prompt prompt;
  prompt.title = forza::ReadWide(base, ctx.r6.u32);
  prompt.description = forza::ReadWide(base, ctx.r7.u32);
  if (prompt.title.empty()) std::swap(prompt.title, prompt.description);
  if (prompt.title.empty()) prompt.title = "Keyboard";
  prompt.max = units - 1;
  prompt.text = forza::ReadWide(base, ctx.r5.u32);
  while (forza::Utf16Length(prompt.text) > prompt.max) forza::PopUtf8(prompt.text);
  REXLOG_INFO("Keyboard UI: \"{}\" ({}), default \"{}\", at most {}, flags {:#x}", prompt.title,
              prompt.description, prompt.text, prompt.max, ctx.r4.u32);

  auto run = [prompt, base, buffer, units](uint32_t& extended_error,
                                           uint32_t& length) -> X_RESULT {
    std::string text;
    if (forza::RunPrompt(prompt, text)) {
      forza::WriteWide(base, buffer, units, text);
      extended_error = X_ERROR_SUCCESS;
    } else {
      extended_error = X_ERROR_CANCELLED;
    }
    length = 0;
    return X_ERROR_SUCCESS;
  };
  auto pre = [] { rex::system::kernel_state()->BroadcastNotification(forza::kNotifySystemUi, 1); };
  auto post = [] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rex::system::kernel_state()->BroadcastNotification(forza::kNotifySystemUi, 0);
    forza::busy = false;
  };
  kernel->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
  ctx.r3.u64 = X_ERROR_IO_PENDING;
}

// BOOL XamIsUIActive(): the SDK's dialogs, or a prompt here.
extern "C" REX_FUNC(__imp__XamIsUIActive) {
  static GuestFunc sdk = forza::SdkImport("__imp__XamIsUIActive");
  if (sdk) sdk(ctx, base);
  if (forza::ui_active) ctx.r3.u64 = 1;
}
