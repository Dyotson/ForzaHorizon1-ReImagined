// Exercise the actual title selection hook and transform code with synthetic
// guest memory. No game files, generated functions, runtime or GPU are started.
// This checks the modal/departure lifecycle, not native input repeat or drawing.
#include "../src/hooks/graphics_menu.cpp"

#include <cstdio>

namespace {
using namespace forza::graphics_menu;
constexpr uint32_t kMenu = 0x100;
constexpr float kSpacing = 40;
std::array<uint8_t, 0x8000> memory{};
uint32_t native_calls = 0;
uint32_t benchmark_calls = 0;
bool leave_title = false;

void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
void Unexpected() { Check(false, "unexpected guest/runtime path"); }

float Position(uint32_t address) {
  return std::bit_cast<float>(Read(memory.data(), address));
}
void Position(uint32_t address, float value) {
  Write(memory.data(), address, std::bit_cast<uint32_t>(value));
}
void Setup(unsigned extra_rows) {
  memory.fill(0);
  title_offsets.clear();
  main_menu = kMenu;
  title_layout_pending = false;
  Write(memory.data(), kMenu + 0x260, 12);
  for (unsigned i = 0; i < 3 + extra_rows; ++i) {
    const uint32_t row = 0x1000 + i * 0x400;
    const uint32_t node = row + 0x40, object = row + 0x80;
    const uint32_t child = row + 0x180, child_object = row + 0x1C0;
    Write(memory.data(), row + 0x2C, node);
    Write(memory.data(), node + 4, object);
    Write(memory.data(), object, 0x7000); // transform owner
    Write(memory.data(), object + 4, node);
    Write(memory.data(), node + 0x14, child);
    Write(memory.data(), child + 4, child_object);
    Write(memory.data(), child_object, 0x7000);
    Write(memory.data(), child_object + 4, child);
    for (auto offset : {0x10u, 0x70u, 0xB0u})
      Position(object + offset, -float(i) * kSpacing);
    Position(child_object + 0xB0, -float(i) * kSpacing + 5);
    const float shift = kSpacing * extra_rows;
    title_offsets.push_back({row, node, shift});
    ShiftTitleRow(memory.data(), node, shift);
  }
}
void CheckPositions(unsigned rows, unsigned shift_rows) {
  for (unsigned i = 0; i < rows; ++i) {
    const auto row = 0x1000 + i * 0x400;
    const float expected = (float(shift_rows) - float(i)) * kSpacing;
    for (auto offset : {0x10u, 0x70u, 0xB0u})
      Check(Position(row + 0x80 + offset) == expected,
            "row local, cached and world positions stay aligned");
    Check(Position(row + 0x1C0 + 0xB0) == expected + 5,
          "child text/hit geometry follows its row");
  }
}
void Select(uint32_t id, uint32_t menu = kMenu, uint32_t event = 2) {
  PPCContext ctx{};
  ctx.r3.u32 = menu;
  ctx.r4.u32 = event;
  ctx.r6.u32 = id;
  sub_82836928(ctx, memory.data());
}
} // namespace

// Only the native selection callback is expected to run in this test. A modal
// leaves CSplash in state 12; a departure switches state and owns the old scene.
extern "C" REX_FUNC(__imp__sub_82836928) {
  ++native_calls;
  if (leave_title) Write(base, kMenu + 0x260, 13);
}
extern "C" REX_FUNC(OriginalBind) { Unexpected(); }
extern "C" REX_FUNC(OriginalSelect) { Unexpected(); }
extern "C" REX_FUNC(__imp__sub_82F2A8D0) { Unexpected(); }
extern "C" REX_FUNC(__imp__sub_827303E8) { Unexpected(); }
extern "C" REX_FUNC(__imp__sub_82739190) { Unexpected(); }
extern "C" REX_FUNC(__imp__sub_827142C0) { Unexpected(); }
extern "C" REX_FUNC(__imp__sub_82736B08) { Unexpected(); }
extern "C" REX_FUNC(__imp__sub_8283CB68) { Unexpected(); }
namespace forza::benchmark {
void Show() { ++benchmark_calls; }
void TitleReady(uint32_t, uint32_t) { Unexpected(); }
}
namespace forza::ultrawide {
Extent OutputSize() { Unexpected(); return {}; }
Extent DrawingSize() { Unexpected(); return {}; }
}
namespace forza::frame_time {
int VblankHz() { Unexpected(); return 60; }
void ResetRenderTiming() { Unexpected(); }
}
// graphics_menu.cpp also defines pause/tuning hooks. Keep those paths forbidden
// in this title-only harness, including when the linker retains unused functions.
namespace forza::native_pause_scene {
bool Owns(uint32_t) { Unexpected(); return false; }
bool Page(PPCContext&, uint8_t*, uint32_t, bool) { Unexpected(); return false; }
}
namespace forza::car_tuning_menu {
bool Available(const PPCContext&, uint8_t*, uint32_t) { Unexpected(); return false; }
bool Open(const PPCContext&, uint8_t*, uint32_t) { Unexpected(); return false; }
bool TryBind(PPCContext&, uint8_t*) { Unexpected(); return false; }
bool TryNotify(PPCContext&, uint8_t*) { Unexpected(); return false; }
bool TryDestroy(PPCContext&, uint8_t*) { Unexpected(); return false; }
bool TryInput(PPCContext&, uint8_t*) { Unexpected(); return false; }
void Tick(const PPCContext&, uint8_t*) { Unexpected(); }
}
namespace forza {
void UpdatePerformanceOverlay() { Unexpected(); }
}

int main() {
  for (unsigned extra_rows : {1u, 2u}) {
    Setup(extra_rows);
    const auto shifted = memory;
    // #223: repeatedly select Single Player, Multiplayer and Horizon Rally.
    // Native modal opening/dismissal retains the same title scene and state.
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
      for (uint32_t id : {10u, 11u, 9u}) {
        const auto before = native_calls;
        Select(id);
        Check(native_calls == before + 1, "stock choice forwarded exactly once");
        Check(memory == shifted, "stock modal does not restore or re-shift rows");
        Check(title_offsets.size() == 3 + extra_rows, "modal retains row references");
        CheckPositions(3 + extra_rows, extra_rows);
      }
    }
    const auto before = native_calls;
    Select(kBenchmark);
    Check(benchmark_calls == extra_rows, "benchmark setup dispatched once");
    Check(native_calls == before && memory == shifted,
          "benchmark overlay retains the title and does not call native selection");

    // A real departure must forget the old nodes without traversing/moving them.
    leave_title = true;
    Select(10);
    Check(title_offsets.empty() && !title_layout_pending, "departure forgets title rows");
    CheckPositions(3 + extra_rows, extra_rows);
    RestoreTitle(memory.data());
    CheckPositions(3 + extra_rows, extra_rows);
    leave_title = false;

    // Graphics explicitly restores a live title before replacing it. Repeated
    // restore calls and fresh title lifetimes must not accumulate a translation.
    Setup(extra_rows);
    RestoreTitle(memory.data());
    RestoreTitle(memory.data());
    CheckPositions(3 + extra_rows, 0);
    Check(title_offsets.empty(), "restoring consumes every saved offset");
  }

  Setup(2);
  const auto shifted = memory;
  leave_title = true;
  Select(10, kMenu + 0x400); // another menu's callback cannot discard these rows
  Check(title_offsets.size() == 5, "unrelated menu retains title offsets");
  Write(memory.data(), kMenu + 0x260, 12);
  Select(10, kMenu, 1); // nor can a non-selection event
  Check(title_offsets.size() == 5, "non-selection event retains title offsets");
  Write(memory.data(), kMenu + 0x260, 12);
  Check(memory == shifted, "unrelated callbacks do not move title rows");
  std::puts("title menu: repeated modals, benchmark overlay, departure and restore passed");
}
