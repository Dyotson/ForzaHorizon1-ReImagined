#include "cursor_idle.h"

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace forza {

struct CursorIdle::Impl {
  struct State final : rex::ui::WindowListener, rex::ui::WindowInputListener {
    State(rex::ui::Window* w, rex::ui::ImGuiDrawer* d)
        : window(w), drawer(d), policy(CursorIdlePolicy::Clock::now()) {}

    void Update(bool moved = false) {
      if (!window) return;
      using Visibility = rex::ui::Window::CursorVisibility;
      // Passive HUDs must not keep the pointer visible forever. Interactive
      // dialogs and friends/chat's explicit text-input lease do keep it visible.
      auto& io = drawer->GetIO();
      const bool overlay = window->IsTextInputActive() ||
          (drawer->HasDialogs() && (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput));
      const bool captured = window->IsMouseCaptureRequested();
      const bool hide = policy.Update(CursorIdlePolicy::Clock::now(), moved,
                                      window->HasFocus(), overlay, captured);
      // Mouse-look owns kHidden while captured, including relative mode's
      // recentering fallback. Never reveal it in response to relative motion.
      if (captured && !overlay) return;
      const auto current = window->GetCursorVisibility();
      if (current == Visibility::kAutoHidden ||
          (current == Visibility::kHidden && !hidden_by_us)) return;
      if (hide) {
        window->SetCursorVisibility(Visibility::kHidden);
        hidden_by_us = true;
      } else if (hidden_by_us) {
        window->SetCursorVisibility(Visibility::kVisible);
        hidden_by_us = false;
      }
    }

    void OnMouseMove(rex::ui::MouseEvent& e) override {
      if (e.dx() != 0 || e.dy() != 0 || !have_position || e.x() != x || e.y() != y) {
        Update(true);
      }
      x = e.x();
      y = e.y();
      have_position = true;
    }
    void OnGotFocus(rex::ui::UISetupEvent&) override { Update(true); }
    void OnLostFocus(rex::ui::UISetupEvent&) override { Update(true); }
    void OnClosing(rex::ui::UIEvent&) override { Detach(); }

    void Detach() {
      if (!window) return;
      window->RemoveInputListener(this);
      window->RemoveListener(this);
      if (hidden_by_us && !window->IsMouseCaptureRequested()) {
        window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      }
      window = nullptr;
      drawer = nullptr;
    }

    rex::ui::Window* window;
    rex::ui::ImGuiDrawer* drawer;
    CursorIdlePolicy policy;
    std::atomic<bool> queued{false};
    bool hidden_by_us = false;
    bool have_position = false;
    int32_t x = 0, y = 0;
  };

  Impl(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer)
      : state(std::make_shared<State>(window, drawer)) {
    window->AddListener(state.get());
    window->AddInputListener(state.get(), 0);
    auto* context = &window->app_context();
    // A slow UI loop must not accumulate timer work. Queued callbacks hold a
    // weak reference, so shutdown cannot leave a callback with a dead window.
    timer = std::jthread([weak = std::weak_ptr<State>(state), context](std::stop_token stop) {
      std::mutex mutex;
      std::condition_variable_any wake;
      std::unique_lock lock(mutex);
      while (!stop.stop_requested()) {
        wake.wait_for(lock, stop, std::chrono::milliseconds(100), [] { return false; });
        if (stop.stop_requested()) break;
        auto live = weak.lock();
        if (!live || live->queued.exchange(true)) continue;
        if (!context->CallInUIThreadDeferred([weak] {
              if (auto current = weak.lock()) {
                current->queued.store(false);
                current->Update();
              }
            })) break;
      }
    });
  }

  ~Impl() {
    timer.request_stop();
    timer.join();
    state->Detach();
    state.reset();
  }

  std::shared_ptr<State> state;
  std::jthread timer;
};

CursorIdle::CursorIdle(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  if (window && drawer) impl_ = std::make_unique<Impl>(window, drawer);
}
CursorIdle::~CursorIdle() = default;

}  // namespace forza
