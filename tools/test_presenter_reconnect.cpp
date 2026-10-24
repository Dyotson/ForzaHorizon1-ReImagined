// Checks SDK patch 0290 (#221): when the window is minimized, a present finds the swapchain
// outdated and the reconnection finds no area (a minimized Win32 window's client area is 0x0), so
// the presenter is left unconnected; SDL reports no size change for a minimize or a restore. The
// presenter must connect again and paint when the window is restored (or exposed), and ask for no
// paint at all while it stays minimized. A fake presenter, surface and window, no GPU or display.
// Build (CI, Linux SDK job; the define matches the SDK build's, patch 0043):
//   clang++ -std=c++23 -pthread -DREX_HAS_FIDELITYFX_SDK=1 -I<overlay>/include
//   tools/test_presenter_reconnect.cpp -L<overlay>/lib -lrexruntime -Wl,-rpath,<overlay>/lib
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

int failures = 0;
#define CHECK(c)                                       \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: %s\n", __LINE__, #c); \
      failures++;                                      \
    }                                                  \
  } while (0)

// What the window system reports: the surface's area (0x0 while minimized, as on Windows).
struct Host {
  uint32_t width = 1280;
  uint32_t height = 720;
};

class FakeSurface final : public rex::ui::Surface {
 public:
  explicit FakeSurface(const Host& host) : host_(host) {}
  TypeIndex GetType() const override { return kTypeIndex_XcbWindow; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    width_out = host_.width;
    height_out = host_.height;
    return true;
  }

 private:
  const Host& host_;
};

class FakePresenter final : public rex::ui::Presenter {
 public:
  explicit FakePresenter(bool vsync_implicit)
      : Presenter(HostGpuLossCallback()), vsync_implicit_(vsync_implicit) {}

  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override {
    return rex::ui::Surface::kTypeFlag_XcbWindow;
  }
  bool CaptureGuestOutput(rex::ui::RawImage&) override { return false; }

  // A new guest frame, as the graphics system sends one.
  void Frame() {
    RefreshGuestOutput(1280, 720, 16, 9, [](GuestOutputRefreshContext&) { return true; });
  }

  int connects = 0;
  int disconnects = 0;
  int presents = 0;
  int outdated_presents = 0;
  // The next presents fail as outdated (the swapchain of a minimized Win32 window).
  bool outdated = false;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      rex::ui::Surface&, uint32_t width, uint32_t height, bool, bool& is_vsync_implicit_out) override {
    CHECK(width && height);  // never asked to connect to a surface without area
    ++connects;
    is_vsync_implicit_out = vsync_implicit_;
    return SurfacePaintConnectResult::kSuccess;
  }
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override { ++disconnects; }
  bool RefreshGuestOutputImpl(uint32_t, uint32_t, uint32_t,
                              std::function<bool(GuestOutputRefreshContext&)>, bool&) override {
    return true;
  }
  PaintResult PaintAndPresentImpl(bool) override {
    if (outdated) {
      ++outdated_presents;
      return PaintResult::kNotPresentedConnectionOutdated;
    }
    ++presents;
    return PaintResult::kPresented;
  }

 private:
  bool vsync_implicit_;
};

class FakeAppContext final : public rex::ui::WindowedAppContext {
 public:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class FakeWindow final : public rex::ui::Window {
 public:
  FakeWindow(rex::ui::WindowedAppContext& context, Host& host)
      : Window(context, "test", host.width, host.height), host_(host) {}
  ~FakeWindow() override { EnterDestructor(); }

  // Delivers the paint the presenter asked for, as the platform's paint event does. Returns
  // whether one was pending.
  bool Pump() {
    if (!paint_pending_) {
      return false;
    }
    paint_pending_ = false;
    OnPaint();
    return true;
  }
  // Delivers queued paints until none is left, at most `limit` (a request that renews itself
  // every paint would be a busy loop).
  int PumpAll(int limit = 10) {
    int pumped = 0;
    while (pumped < limit && Pump()) {
      ++pumped;
    }
    return pumped;
  }
  void Minimize() {
    host_.width = 0;
    host_.height = 0;
    WindowDestructionReceiver destruction_receiver(this);
    OnMinimized(destruction_receiver);
  }
  // SDL restores without a size change event: the size is the one from before.
  void Restore(uint32_t width, uint32_t height) {
    host_.width = width;
    host_.height = height;
    WindowDestructionReceiver destruction_receiver(this);
    OnRestored(destruction_receiver);
  }
  void Expose() { OnPaint(true); }

  int paint_requests = 0;

 protected:
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return std::make_unique<FakeSurface>(host_);
  }
  void RequestPaintImpl() override {
    ++paint_requests;
    paint_pending_ = true;
  }

 private:
  Host& host_;
  bool paint_pending_ = false;
};

// Minimize as on Windows: the next present finds the swapchain outdated, and the reconnection
// finds no area. Then the guest keeps sending frames while the window stays minimized.
void MinimizeWhilePresenting(FakeWindow& window, FakePresenter& presenter) {
  const int connects = presenter.connects;
  presenter.outdated = true;
  window.Minimize();
  presenter.Frame();
  window.PumpAll();
  CHECK(presenter.outdated_presents == 1);
  CHECK(presenter.disconnects == 1);
  CHECK(presenter.connects == connects);
  presenter.outdated = false;

  // While minimized: frames and an expose ask for no paint and connect nothing.
  const int requests = window.paint_requests;
  for (int i = 0; i < 5; ++i) {
    presenter.Frame();
  }
  window.Expose();
  CHECK(window.PumpAll() == 0);
  CHECK(window.paint_requests == requests);
  CHECK(presenter.connects == connects);
}

void Scenario(bool vsync_implicit, bool restore_event) {
  std::printf("vsync %s, %s\n", vsync_implicit ? "implicit (FIFO)" : "not implicit",
              restore_event ? "restore event" : "expose only");
  Host host;
  FakeAppContext context;
  auto presenter = std::make_unique<FakePresenter>(vsync_implicit);
  auto window = std::make_unique<FakeWindow>(context, host);
  window->SetPresenter(presenter.get());
  CHECK(window->Open());
  CHECK(presenter->connects == 1);
  window->PumpAll();
  presenter->Frame();
  window->PumpAll();
  const int presents = presenter->presents;
  CHECK(presents >= 1);

  MinimizeWhilePresenting(*window, *presenter);

  if (restore_event) {
    window->Restore(1280, 720);
  } else {
    // Without the restore event, the window system's expose after the restore.
    host.width = 1280;
    host.height = 720;
    window->Expose();
  }
  CHECK(presenter->connects == 2);
  CHECK(presenter->presents == presents + 1);  // the restored window shows the last frame
  CHECK(window->PumpAll() < 10);

  // Live again: a new frame is presented.
  const int presents_restored = presenter->presents;
  presenter->Frame();
  window->PumpAll();
  CHECK(presenter->presents == presents_restored + 1);

  // A restore with the connection intact (another platform, or no present while minimized)
  // keeps the swapchain and just paints.
  window->Restore(1280, 720);
  CHECK(presenter->connects == 2);
  CHECK(presenter->disconnects == 1);

  window.reset();
  presenter.reset();
}

}  // namespace

int main() {
  Scenario(true, true);
  Scenario(true, false);
  Scenario(false, true);
  Scenario(false, false);
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("presenter reconnect: ok\n");
  return 0;
}
