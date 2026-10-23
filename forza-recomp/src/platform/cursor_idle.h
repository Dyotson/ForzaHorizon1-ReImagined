#pragma once

#include <chrono>
#include <memory>

namespace rex::ui {
class Window;
class ImGuiDrawer;
}

namespace forza {

// The clock is supplied by the caller so focus/capture/overlay transitions can
// be checked without a window or a running game.
class CursorIdlePolicy {
 public:
  using Clock = std::chrono::steady_clock;
  explicit CursorIdlePolicy(Clock::time_point now) : last_activity_(now) {}
  bool Update(Clock::time_point now, bool moved, bool focused,
              bool overlay, bool captured) {
    if (moved || !focused || overlay || captured || was_captured_) last_activity_ = now;
    was_captured_ = captured;
    return focused && !overlay && !captured && now - last_activity_ >= std::chrono::seconds(3);
  }

 private:
  Clock::time_point last_activity_;
  bool was_captured_ = false;
};

// Owned by ForzaApp; all window/ImGui access stays on the UI thread.
class CursorIdle {
 public:
  CursorIdle(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer);
  ~CursorIdle();
  CursorIdle(const CursorIdle&) = delete;
  CursorIdle& operator=(const CursorIdle&) = delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace forza
