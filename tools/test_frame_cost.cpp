// The slow-frame report of the GPU command processor (#45): src/graphics/fh1_frame_cost.h of
// the patched SDK source (patch 0180). Which frames are slow, when a line is logged and what it
// names as the cause; and the once-a-minute summary (patch 0200).
// Build with the patched SDK source's src/graphics on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/graphics
//       tools/test_frame_cost.cpp -o test_frame_cost
#include "fh1_frame_cost.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace cost = rex::graphics::fh1_frame_cost;

#define CHECK(x)                                                \
  do {                                                          \
    if (!(x)) {                                                 \
      std::fprintf(stderr, "frame cost: FAIL: %s\n", #x);       \
      std::exit(1);                                             \
    }                                                           \
  } while (false)

namespace {

constexpr uint64_t kMB = uint64_t(1) << 20;

cost::Frame Typical(double ms = 33.3) {
  cost::Frame f;
  f.ms = ms;
  f.cpu_ms = 8;
  f.gpu_wait_ms = 1;
  f.draws = 4000;
  f.texture_loads = 2;
  f.texture_load_bytes = kMB;
  f.texture_load_ms = 0.1;
  f.upload_bytes = 2 * kMB;
  f.upload_ms = 0.3;
  return f;
}

// Feeds frames until `until` seconds; returns the number of lines and keeps the last.
struct Run {
  cost::Report report;
  double t = 0;
  int lines = 0;
  std::string last;
  cost::Cause last_cause = cost::Cause::kUnclear;
  void Feed(const cost::Frame& f, double until) {
    while (t < until) {
      t += f.ms / 1000.0;
      if (report.OnFrame(f, t)) {
        last = report.Line(t);
        last_cause = report.MainCause();
        report.Taken();
        ++lines;
      }
    }
  }
  void One(const cost::Frame& f) {
    t += f.ms / 1000.0;
    if (report.OnFrame(f, t)) {
      last = report.Line(t);
      last_cause = report.MainCause();
      report.Taken();
      ++lines;
    }
  }
};

bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

int main() {
  // Steady frames: nothing to report.
  {
    Run run;
    run.Feed(Typical(), 300);
    CHECK(run.lines == 0);
    CHECK(run.report.typical_ms() > 33 && run.report.typical_ms() < 34);
  }
  // Slow frames while the game loads (the first 30 s) are not reported.
  {
    Run run;
    cost::Frame slow = Typical(120);
    slow.pipelines = 3;
    slow.pipeline_ms = 60;
    run.Feed(slow, 29);
    run.Feed(Typical(), 45);
    CHECK(run.lines == 0);
  }
  // A burst of pipeline creations: one line 10 s after the first slow frame, naming them.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame slow = Typical(90);
    slow.cpu_ms = 70;
    slow.pipelines = 2;
    slow.pipeline_ms = 55;
    slow.shaders = 1;
    slow.shader_ms = 4;
    for (int i = 0; i < 5; ++i) run.One(slow);
    CHECK(run.report.slow_count() == 5);
    run.Feed(Typical(), 69);
    CHECK(run.lines == 0);
    run.Feed(Typical(), 71);
    CHECK(run.lines == 1);
    CHECK(run.last_cause == cost::Cause::kShaders);
    CHECK(Has(run.last, "GPU slow frames: 5 of "));
    CHECK(Has(run.last, "worst 90 ms"));
    CHECK(Has(run.last, "pipelines 2.0 (55.0 ms)"));
    CHECK(Has(run.last, "cause: shaders and pipelines"));
    // Nothing more without new slow frames.
    run.Feed(Typical(), 200);
    CHECK(run.lines == 1);
  }
  // At most one line per 10 s while frames keep being slow.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame f = Typical();
    int frame = 0;
    while (run.t < 120) {
      cost::Frame g = f;
      if (++frame % 10 == 0) {
        g.ms = 80;
        g.cpu_ms = 50;
        g.texture_loads = 300;
        g.texture_load_bytes = 90 * kMB;
        g.texture_load_ms = 12;
        g.textures_created = 40;
        g.texture_created_bytes = 60 * kMB;
        g.texture_create_ms = 9;
      }
      run.One(g);
    }
    CHECK(run.lines >= 5 && run.lines <= 6);
    CHECK(run.last_cause == cost::Cause::kTextures);
    CHECK(Has(run.last, "40.0 new (60.0 MB, 9.0 ms), 300.0 loaded (90.0 MB, 12.0 ms)"));
  }
  // The threshold follows the typical frame: at 20 fps (50 ms) a 60 ms frame is not slow, 80 is.
  {
    Run run;
    run.Feed(Typical(50), 60);
    CHECK(run.report.SlowThresholdMs() > 74 && run.report.SlowThresholdMs() < 76);
    run.One(Typical(60));
    CHECK(run.report.slow_count() == 0);
    run.One(Typical(80));
    CHECK(run.report.slow_count() == 1);
  }
  // At 30 fps the threshold is 50 ms.
  {
    Run run;
    run.Feed(Typical(), 60);
    CHECK(run.report.SlowThresholdMs() == cost::Report::kSlowMs);
    run.One(Typical(49));
    CHECK(run.report.slow_count() == 0);
    run.One(Typical(51));
    CHECK(run.report.slow_count() == 1);
  }
  // The command processor idle in slow frames: the game was late.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame late = Typical(150);
    late.cpu_ms = 9;
    late.guest_wait_ms = 100;
    run.One(late);
    run.Feed(Typical(), 75);
    CHECK(run.lines == 1);
    CHECK(run.last_cause == cost::Cause::kGame);
  }
  // Twice the draws at the same cost per draw: more draws.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame busy = Typical(60);
    busy.draws = 9000;
    busy.cpu_ms = 40;
    busy.gpu_wait_ms = 2;
    busy.ms = 55;
    for (int i = 0; i < 20; ++i) run.One(busy);
    run.Feed(Typical(), 75);
    CHECK(run.lines == 1);
    CHECK(run.last_cause == cost::Cause::kDraws);
  }
  // Waits for the GPU (a GPU-bound frame, or a readback).
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame wait = Typical(70);
    wait.gpu_wait_ms = 45;
    run.One(wait);
    run.Feed(Typical(), 75);
    CHECK(run.lines == 1);
    CHECK(run.last_cause == cost::Cause::kGpuWaits);
  }
  // Render targets made again (e.g. after a trim) and guest memory uploads.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame rt = Typical(70);
    rt.cpu_ms = 40;
    rt.render_targets = 6;
    rt.render_target_bytes = 120 * kMB;
    rt.render_target_ms = 20;
    run.One(rt);
    run.Feed(Typical(), 75);
    CHECK(run.lines == 1);
    CHECK(run.last_cause == cost::Cause::kRenderTargets);
    cost::Frame up = Typical(70);
    up.cpu_ms = 40;
    up.upload_bytes = 200 * kMB;
    up.upload_ms = 25;
    run.One(up);
    run.Feed(Typical(), 90);
    CHECK(run.lines == 2);
    CHECK(run.last_cause == cost::Cause::kUploads);
    CHECK(Has(run.last, "uploads 200.0 MB (25.0 ms)"));
  }
  // Back from the map: the scene's resolves copied back to guest memory, each after a GPU wait.
  // The waits count as readbacks, not as waits for the GPU.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame back = Typical(60);
    back.cpu_ms = 30;
    back.gpu_wait_ms = 11;
    back.readbacks = 10;
    back.readback_bytes = 24 * kMB;
    back.readback_ms = 22;
    back.readback_wait_ms = 10;
    run.One(back);
    run.Feed(Typical(), 75);
    CHECK(run.lines == 1);
    CHECK(run.last_cause == cost::Cause::kReadbacks);
    CHECK(Has(run.last, "resolve readbacks 10.0 (24.0 MB, 22.0 ms, 10.0 ms GPU waits)"));
  }
  // Nothing large enough to name.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame f = Typical(55);
    f.cpu_ms = 30;
    run.One(f);
    run.Feed(Typical(), 75);
    CHECK(run.lines == 1);
    CHECK(run.last_cause == cost::Cause::kUnclear);
  }
  // After kMaxLines lines, one a minute.
  {
    Run run;
    run.Feed(Typical(), 60);
    cost::Frame slow = Typical(80);
    slow.gpu_wait_ms = 40;
    int frame = 0;
    while (run.lines < int(cost::Report::kMaxLines)) {
      run.One(++frame % 30 == 0 ? slow : Typical());
    }
    const double at = run.t;
    while (run.t < at + 59) run.One(++frame % 30 == 0 ? slow : Typical());
    CHECK(run.lines == int(cost::Report::kMaxLines));
    while (run.t < at + 62) run.One(++frame % 30 == 0 ? slow : Typical());
    CHECK(run.lines == int(cost::Report::kMaxLines) + 1);
  }
  // Frame::Add sums every counter.
  {
    cost::Frame a = Typical(), b = Typical();
    b.render_target_trims = 1;
    b.textures_evicted = 7;
    b.texture_evicted_bytes = 3 * kMB;
    b.readbacks = 2;
    b.readbacks_skipped = 5;
    b.readback_wait_ms = 1.5;
    a.Add(b);
    CHECK(a.draws == 8000 && a.render_target_trims == 1 && a.textures_evicted == 7);
    CHECK(a.readbacks == 2 && a.readbacks_skipped == 5 && a.readback_wait_ms == 1.5);
    CHECK(a.texture_evicted_bytes == 3 * kMB && a.upload_bytes == 4 * kMB);
  }
  // The minute summary (#45, patch 0200): one line a minute with the minute's frames and the
  // sizes now; the next minute counts only its own frames and watch checks.
  {
    cost::Summary summary;
    cost::Snapshot s;
    s.textures = 1873;
    s.texture_bytes = 312 * kMB;
    s.render_targets = 39;
    s.render_target_bytes = 553 * kMB;
    s.shaders = 1650;
    s.pipelines = 1185;
    s.framebuffers = 120;
    s.watch_ranges = 3700;
    s.watch_nodes = 5200;
    s.watch_nodes_visited = 1000;
    s.watch_fires = 10;
    s.readback_buffers = 12;
    s.readback_buffer_bytes = 25 * kMB;
    s.readback_keys = 64;
    s.guest_virtual_bytes = 1830 * kMB;
    s.guest_physical_bytes = 410 * kMB;
    summary.SetStart(1000, 10);
    double t = 0;
    int lines = 0;
    std::string line;
    auto feed = [&](const cost::Frame& f, double until) {
      while (t < until) {
        t += f.ms / 1000.0;
        if (summary.OnFrame(f, t)) {
          line = summary.Line(t, s);
          ++lines;
        }
      }
    };
    cost::Frame f = Typical(100.0 / 3.0);
    f.guest_wait_ms = 20;
    f.textures_created = 1;
    feed(f, 30);
    cost::Frame slow = Typical(100);
    feed(slow, 30.05);
    s.watch_nodes_visited = 1000 + 1801 * 300;
    s.watch_fires = 10 + 1801 * 2;
    feed(f, 59.9);
    CHECK(lines == 0);
    feed(f, 60.1);
    CHECK(lines == 1);
    CHECK(Has(line, "GPU minute 1: 30.0 fps (1% low 27."));
    CHECK(Has(line, "worst 100 ms"));
    CHECK(Has(line, "command processor 8.0 ms CPU (2.00 us a draw), 1.0 ms GPU waits, 20.0 ms game "
                    "waits, 4000 draws"));
    CHECK(Has(line, "2.00 MB uploads"));
    CHECK(Has(line, "300 memory watch nodes checked (2.0 writes)"));
    CHECK(Has(line, "a second: 30.0 textures new, 60.0 loaded, 0.0 evicted"));
    CHECK(Has(line, "textures 1873 (312 MB), render targets 39 (553 MB), shaders 1650, pipelines 1185"));
    CHECK(Has(line, "framebuffers 120, memory watches 3700 (5200 nodes)"));
    CHECK(Has(line, "readback buffers 12 (25 MB, 64 keys)"));
    CHECK(Has(line, "guest memory 1830 MB virtual + 410 MB physical"));
    CHECK(!Has(line, "GPU memory") && !Has(line, "resident") && !Has(line, "clocks"));
    // The next minute: 20 fps, no watch checks, the device's memory and the process known.
    s.device_local_usage = 1600 * kMB;
    s.device_local_budget = 8000 * kMB;
    s.system_usage = 300 * kMB;
    s.process_resident_bytes = 2450 * kMB;
    s.cpu_ghz = 2.8;
    s.gpu_ghz = 1.6;
    s.gpu_celsius = 72;
    s.gpu_watts = 14.84;
    feed(Typical(50), 120.2);
    CHECK(lines == 2);
    CHECK(Has(line, "GPU minute 2: 20.0 fps (1% low 20.0, worst 50 ms)"));
    CHECK(Has(line, "0 memory watch nodes checked (0.0 writes)"));
    CHECK(Has(line, "GPU memory 1600 of 8000 MB device-local, 300 MB system, process 2450 MB "
                    "resident; clocks: CPU up to 2.80 GHz, GPU 1.60 GHz 72 C 14.8 W"));
    // Only the GPU's clock known.
    s.cpu_ghz = 0;
    s.gpu_celsius = 0;
    s.gpu_watts = 0;
    feed(Typical(50), 180.3);
    CHECK(lines == 3);
    CHECK(Has(line, "resident; clocks: GPU 1.60 GHz") && !Has(line, " C ") && !Has(line, " W"));
  }
  std::printf("frame cost: all checks passed\n");
  return 0;
}
