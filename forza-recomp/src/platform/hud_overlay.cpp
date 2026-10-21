// forza - the friends, chat and invite overlay; see hud_overlay.h.

#include "hud_overlay.h"
#include "performance_history.h"
#include "sdk_import.h"
#include "benchmark.h"
#include <rex/cvar.h>
#include <string_view>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>

REXCVAR_DEFINE_STRING(forza_performance_overlay, "off", "Display",
    "Performance overlay: off, fps, or graph (game-frame timing)")
    .validator([](std::string_view value) {
      return value == "off" || value == "fps" || value == "graph";
    })
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace forza {
namespace {

using Clock = std::chrono::steady_clock;
ImFont* performance_font = nullptr;

// UI thread only. Removal forgets the hint, so restore it on each registration.
void AddPassiveDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImGuiDialog* dialog) {
  drawer->AddDialog(dialog);
  using SetRepaint = void (*)(const void*, bool);
  static const auto set_repaint = reinterpret_cast<SetRepaint>(
      SdkSymbol("rexui_SetDialogContinuousRepaint"));
  if (set_repaint) set_repaint(dialog, false);
}

class HudDialog final : public rex::ui::ImGuiDialog {
 public:
  HudDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context,
            rex::ui::Window* window)
      : ImGuiDialog(drawer), drawer_(drawer), app_context_(app_context), window_(window) {
    drawer_->RemoveDialog(this);  // the constructor registered it; registered while shown
  }

  void Show(const std::string& title, const std::string& body, double seconds) {
    {
      std::lock_guard lock(mutex_);
      title_ = title;
      body_ = body;
      until_ = Clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000));
    }
    if (!registered_.exchange(true)) {
      app_context_.CallInUIThread([this] { drawer_->AddDialog(this); });
    }
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    until_ = Clock::time_point{};
  }

  void WantKeys(bool on) {
    {
      std::lock_guard lock(mutex_);
      if (want_keys_ == on) return;
      want_keys_ = on;
      keys_ = {};
    }
    // SDL text input: UI thread only. Without it no text arrives, keys still do.
    if (window_) app_context_.CallInUIThread([w = window_, on] { w->SetTextInputActive(on); });
  }

  HudKeys TakeKeys() {
    std::lock_guard lock(mutex_);
    HudKeys keys = keys_;
    keys_.typed.clear();
    keys_.backspace = 0;
    keys_.enter = keys_.escape = keys_.up = keys_.down = keys_.left = keys_.right = false;
    return keys;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::string title, body;
    {
      std::lock_guard lock(mutex_);
      if (Clock::now() >= until_) {
        // The drawer detaches from the presenter once its last dialog is gone.
        registered_ = false;
        drawer_->RemoveDialog(this);
        return;
      }
      title = title_;
      body = body_;
      if (want_keys_) CollectKeys(io);
    }
    const float scale = std::clamp(io.DisplaySize.y / 720.0f, 1.0f, 2.5f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.06f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 10.0f * scale));
    if (ImGui::Begin("##forza_hud", nullptr, flags)) {
      ImGui::SetWindowFontScale(1.5f * scale);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.1f, 1.0f));
      ImGui::TextUnformatted(title.c_str());
      ImGui::PopStyleColor();
      ImGui::SetWindowFontScale(1.25f * scale);
      size_t start = 0;
      while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        if (body[start] == kHudHighlight) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
          ImGui::TextUnformatted(body.data() + start + 1, body.data() + end);
          ImGui::PopStyleColor();
        } else {
          ImGui::TextUnformatted(body.data() + start, body.data() + end);
        }
        start = end + 1;
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  // This frame's keyboard input (mutex_ held). The drawer feeds ImGui the window's key and
  // text events while any dialog is registered.
  void CollectKeys(ImGuiIO& io) {
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
      const unsigned c = io.InputQueueCharacters[i];
      if (c < 0x20 || c == 0x7F) continue;
      if (c < 0x80) {
        keys_.typed += char(c);
      } else if (c < 0x800) {
        keys_.typed += char(0xC0 | c >> 6);
        keys_.typed += char(0x80 | (c & 0x3F));
      } else {
        keys_.typed += char(0xE0 | c >> 12);
        keys_.typed += char(0x80 | (c >> 6 & 0x3F));
        keys_.typed += char(0x80 | (c & 0x3F));
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) ++keys_.backspace;
    keys_.enter |= ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    keys_.escape |= ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    keys_.up |= ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    keys_.down |= ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    keys_.left |= ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
    keys_.right |= ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {  // keyboard keys
      if (ImGui::IsKeyDown(ImGuiKey(k))) {
        ++keys_.key_frames;
        break;
      }
    }
  }

  rex::ui::ImGuiDrawer* drawer_;
  rex::ui::WindowedAppContext& app_context_;
  rex::ui::Window* window_;
  std::mutex mutex_;
  bool want_keys_ = false;
  HudKeys keys_;
  std::string title_, body_;
  Clock::time_point until_{};
  std::atomic<bool> registered_{false};
};

class PerformanceDialog final : public rex::ui::ImGuiDialog {
 public:
  PerformanceDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& context)
      : ImGuiDialog(drawer), drawer_(drawer), context_(context) {
    drawer_->RemoveDialog(this);
  }
  void Update() {
    const auto value = REXCVAR_GET(forza_performance_overlay);
    const int mode = value == "graph" ? 2 : value == "fps" ? 1 : 0;
    const int previous = mode_.exchange(mode);
    if (previous == mode) return;
    if (!previous) {
      std::lock_guard lock(mutex_);
      history_ = PerformanceHistory{};
    }
    context_.CallInUIThread([this] {
      if (mode_.load()) {
        AddPassiveDialog(drawer_, this);
      } else {
        drawer_->RemoveDialog(this);
      }
    });
  }
  void Frame() {
    if (!mode_.load()) return;
    std::lock_guard lock(mutex_);
    history_.Record(Now());
  }
 protected:
  void OnDraw(ImGuiIO& io) override {
    const int mode = mode_.load();
    if (!mode) return;
    std::array<float, PerformanceHistory::kCapacity> ms;
    size_t count;
    double fps;
    {
      std::lock_guard lock(mutex_);
      ms = history_.Ordered();
      count = history_.count();
      fps = history_.Fps(Now());
    }
    const float scale = std::clamp(io.DisplaySize.y / 1080.0f, 1.0f, 2.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 24 * scale, 24 * scale),
                            ImGuiCond_Always, ImVec2(1, 0));
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (performance_font) ImGui::PushFont(performance_font);
    if (ImGui::Begin("##forza_performance", nullptr, flags)) {
      ImGui::SetWindowFontScale((performance_font ? 1.0f : 2.6f) * scale);
      char label[32];
      std::snprintf(label, sizeof(label), "%.0f FPS", fps);
      // Keep the counter at the right edge, also when the optional graph is wider.
      ImGui::SetCursorPosX(std::max(0.0f, ImGui::GetWindowWidth() -
                                          ImGui::CalcTextSize(label).x));
      ImGui::TextUnformatted(label);
      if (mode == 2) {
        ImGui::SetWindowFontScale((performance_font ? 0.65f : 1.7f) * scale);
        const float latest = count ? ms[count - 1] : 0;
        const float peak = count ? *std::max_element(ms.begin(), ms.begin() + count) : 0;
        ImGui::Text("%.1f ms   peak %.1f ms", latest, peak);
        // Auto-range retains long hitches instead of clipping them at 33 ms.
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PlotLines("##frame_times", ms.data(), int(count), 0, nullptr,
                         0, std::max(33.3f, peak * 1.1f), ImVec2(270 * scale, 70 * scale));
        ImGui::PopStyleColor();
      }
    }
    ImGui::End();
    if (performance_font) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
  }
 private:
  static double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
  }
  rex::ui::ImGuiDrawer* drawer_;
  rex::ui::WindowedAppContext& context_;
  std::atomic<int> mode_{0};
  std::mutex mutex_;
  PerformanceHistory history_;
};
PerformanceDialog* performance = nullptr;

class BenchmarkDialog final : public rex::ui::ImGuiDialog {
public:
  BenchmarkDialog(rex::ui::ImGuiDrawer *drawer,
                  rex::ui::WindowedAppContext &context)
      : ImGuiDialog(drawer), drawer_(drawer), context_(context) {
    drawer_->RemoveDialog(this);
  }
  void Update() {
    const bool visible =
        benchmark::Snapshot().screen != benchmark::Screen::hidden;
    if (visible_.exchange(visible) == visible)
      return;
    context_.CallInUIThread([this] {
      if (visible_.load())
        AddPassiveDialog(drawer_, this);
      else
        drawer_->RemoveDialog(this);
    });
  }

protected:
  void OnDraw(ImGuiIO &io) override {
    const auto view = benchmark::Snapshot();
    if (view.screen == benchmark::Screen::hidden)
      return;
    const bool running = view.screen == benchmark::Screen::running;
    const bool results = view.screen == benchmark::Screen::results;
    if (results) {
      // The native pause menu stops the race; cover its choices while displaying
      // the benchmark report so they cannot compete with the report's actions.
      ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                   IM_COL32(10, 10, 13, 255));
    }
    const float scale = std::clamp(io.DisplaySize.y / 1080.f, 0.75f, 2.f);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f,
               io.DisplaySize.y * (running ? 0.055f : 0.5f)),
        ImGuiCond_Always, ImVec2(0.5f, running ? 0.f : 0.5f));
    ImGui::SetNextWindowSize(ImVec2((running ? 520 : 830) * scale, 0),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(running ? 0.8f : 0.96f);
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(30 * scale, 24 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(18 * scale, 18 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.035f, 0.045f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.93f, 0, 0.32f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 0.08f, 0.4f, 1));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.93f, 0, 0.32f, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.93f, 0, 0.32f, 1));
    if (performance_font)
      ImGui::PushFont(performance_font);
    if (ImGui::Begin("##benchmark", nullptr, flags)) {
      ImGui::SetWindowFontScale((performance_font ? 1.f : 2.1f) * scale);
      ImGui::TextColored(ImVec4(1, 0.05f, 0.38f, 1),
                         results ? "BENCHMARK RESULTS" : "BENCHMARK");
      ImGui::Separator();
      if (running) {
        ImGui::TextUnformatted(view.status.c_str());
        if (view.elapsed > 0) {
          ImGui::SetWindowFontScale((performance_font ? 0.7f : 1.6f) * scale);
          ImGui::ProgressBar(
              float(std::clamp(view.elapsed / view.duration, 0.0, 1.0)),
              ImVec2(-1, 8 * scale), "");
          ImGui::Text("%.0f / %.0f seconds", view.elapsed, view.duration);
        }
        ImGui::SetWindowFontScale((performance_font ? 0.65f : 1.5f) * scale);
        ImGui::TextDisabled("B  CANCEL");
      } else if (results) {
        ImGui::SetWindowFontScale((performance_font ? 0.7f : 1.6f) * scale);
        ImGui::TextWrapped("%s", view.status.c_str());
        ImGui::TextDisabled("%s", view.settings.c_str());
        if (view.valid) {
          ImGui::Spacing();
          ImGui::Columns(3, nullptr, false);
          ImGui::TextDisabled("AVERAGE");
          ImGui::Text("%.1f FPS", view.stats.fps);
          ImGui::NextColumn();
          ImGui::TextDisabled("1%% LOW");
          ImGui::Text("%.1f FPS", view.stats.low1);
          ImGui::NextColumn();
          ImGui::TextDisabled("P99 FRAME TIME");
          ImGui::Text("%.1f ms", view.stats.p99_ms);
          ImGui::Columns(1);
          ImGui::Spacing();
          ImGui::TextDisabled("FRAME TIME / MILLISECONDS");
          ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1, 0.06f, 0.36f, 1));
          ImGui::PlotLines("##benchmark-frames", view.frame_ms.data(),
                           int(view.frame_ms.size()), 0, nullptr, 0,
                           float(std::max(33.3, view.stats.worst_ms * 1.05)),
                           ImVec2(-1, 125 * scale));
          ImGui::PopStyleColor();
          ImGui::Text("%.1f seconds  |  %zu frames  |  %zu stalls over 50 ms",
                      view.stats.seconds, view.stats.frames,
                      view.stats.stalls50);
          ImGui::TextWrapped(
              "Reports saved in benchmarks/%s",
              std::filesystem::path(view.folder).filename().string().c_str());
        }
      } else {
        ImGui::SetWindowFontScale((performance_font ? 0.8f : 1.8f) * scale);
        ImGui::TextUnformatted("RECARO RUSH");
        ImGui::TextDisabled("%s", view.settings.c_str());
        ImGui::SetWindowFontScale((performance_font ? 0.65f : 1.5f) * scale);
        ImGui::TextWrapped("The game's AI drives a fixed festival race using "
                           "your current car and graphics settings.");
        ImGui::Spacing();
        ImGui::TextUnformatted("15 seconds settling  /  60 seconds measured");
        ImGui::TextWrapped("Career progress stays in a separate save copy. Run "
                           "once to warm shaders before comparing results.");
        ImGui::TextWrapped("Use the same car, camera and difficulty for "
                           "comparisons. AI traffic and collisions can vary.");
        if (view.status != "Recaro Rush")
          ImGui::TextWrapped("%s", view.status.c_str());
      }
      if (!running) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetWindowFontScale((performance_font ? 0.7f : 1.6f) * scale);
        if (ImGui::Button(results ? "A  RUN AGAIN" : "A  RUN BENCHMARK",
                          ImVec2(280 * scale, 50 * scale)))
          benchmark::Run();
        ImGui::SameLine();
        if (ImGui::Button("B  BACK", ImVec2(160 * scale, 50 * scale)))
          benchmark::Close();
      }
    }
    ImGui::End();
    if (performance_font)
      ImGui::PopFont();
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(4);
  }

private:
  rex::ui::ImGuiDrawer *drawer_;
  rex::ui::WindowedAppContext &context_;
  std::atomic<bool> visible_{false};
};
BenchmarkDialog *benchmark_dialog = nullptr;

// Lives for the whole run (the drawer keeps a raw pointer while it is registered).
HudDialog* hud = nullptr;

}  // namespace

void ConfigurePerformanceFont(ImFontAtlas* atlas) {
  performance_font = nullptr;
  // The game's Horizon fonts use its proprietary vector renderer, not TTF.
  // Use an installed bold face rather than distributing a licensed font asset.
  std::filesystem::path path;
#ifdef _WIN32
  if (const char* windows = std::getenv("WINDIR"))
    path = std::filesystem::path(windows) / "Fonts" / "arialbd.ttf";
#elif defined(__APPLE__)
  path = "/System/Library/Fonts/Supplemental/Arial Bold.ttf";
#else
  path = "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf";
#endif
  std::error_code ec;
  if (!path.empty() && std::filesystem::is_regular_file(path, ec)) {
    ImFontConfig config;
    config.OversampleH = config.OversampleV = 2;
    performance_font = atlas->AddFontFromFileTTF(path.string().c_str(), 26, &config);
  }
}

void InstallHud(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext& app_context,
                rex::ui::Window* window) {
  if (!drawer || hud) return;
  hud = new HudDialog(drawer, app_context, window);
  performance = new PerformanceDialog(drawer, app_context);
  performance->Update();
  benchmark_dialog=new BenchmarkDialog(drawer,app_context);
  benchmark_dialog->Update();
}

void HudShow(const std::string& title, const std::string& body, double seconds, bool log) {
  if (log) REXLOG_INFO("HUD: {} | {}", title, body);
  if (hud) hud->Show(title, body, seconds);
}

void PerformanceOverlayFrame() { if (performance) performance->Frame(); }
void UpdatePerformanceOverlay() {
  if (performance) performance->Update();
  if (benchmark_dialog) benchmark_dialog->Update();
}

void HudClear() {
  if (hud) hud->Clear();
}

void HudWantKeys(bool on) {
  if (hud) hud->WantKeys(on);
}

HudKeys HudTakeKeys() { return hud ? hud->TakeKeys() : HudKeys{}; }

}  // namespace forza
