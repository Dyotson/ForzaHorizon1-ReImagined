#include "../src/platform/fh2_tab_layout.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <vector>

struct Native {
  uint8_t* base;
  unsigned calls = 0;
  std::array<uint32_t, 4> queried_alignments{};
  std::array<float, 4> placed_text_x{};
  void Call(uint32_t address, std::initializer_list<uint32_t> args) {
    assert((address == 0x828D1E08 || address == 0x828CFAB8) &&
           args.size() == 1 && *args.begin() == 0x100);
    ++calls;
    if (address != 0x828CFAB8) return;
    using namespace forza::fh2_tab_layout::detail;
    const auto controller = *args.begin();
    const auto scalar = [&](uint32_t at) { return std::bit_cast<float>(Read(base, at)); };
    for (unsigned i = 0; i < 4; ++i) {
      const auto slot = 3 * i;
      // Model the native distinction: 378 gates the entire text setter.
      if (!Read(base, Slot(controller, slot) + 4) ||
          !forza::GuestPtr(base, controller)[0x378]) continue;
      const auto prefix = scalar(controller + 0x1B0 + slot * 4);
      const auto width_delta = scalar(controller + 0x1B4 + slot * 4) - prefix;
      const auto adjustment = queried_alignments[i] == 1 ? .5f * width_delta :
                              queried_alignments[i] == 2 ? width_delta : 0.f;
      placed_text_x[i] = scalar(Slot(controller, slot) + 12) + prefix + adjustment;
    }
  }
};

int main() {
  using namespace forza::fh2_tab_layout;
  using namespace forza::fh2_tab_layout::detail;
  std::array<uint8_t, 0x3000> memory{};
  Native ui{memory.data()};
  constexpr uint32_t controller = 0x100, rb = 0x2300;
  constexpr uint32_t fourth = 0x2100, sixth = 0x2200;
  Write(ui.base, controller, 0x820814AC);
  Write(ui.base, controller + 0x1AC, 19);
  Write(ui.base, controller + 0x370, 6);
  for (unsigned i = 0; i < 20; ++i) {
    const auto slot = Slot(controller, i), binding = 0x1000 + 0x20 * i;
    Write(ui.base, slot, binding);
    Write(ui.base, slot + 8, 100 + i);
    Write(ui.base, slot + 12, std::bit_cast<uint32_t>(float(i * 10)));
    Write(ui.base, binding + 4, 0x2000 + i * 4);
  }
  Write(ui.base, Read(ui.base, Slot(controller, 11)) + 4, fourth);
  Write(ui.base, Read(ui.base, Slot(controller, 17)) + 4, sixth);
  Write(ui.base, Read(ui.base, Slot(controller, 18)) + 4, rb);
  const auto original = memory;
  assert(UseFourTabs(ui, controller, rb + 4, fourth, sixth) == Result::NotReady);
  assert(memory == original && ui.calls == 0);
  Write(ui.base, Slot(controller, 18) + 12,
        std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()));
  assert(UseFourTabs(ui, controller, rb, fourth, sixth) == Result::NotReady);
  memory = original;
  assert(UseFourTabs(ui, controller, rb, fourth, sixth) == Result::Applied);
  assert(ui.calls == 1 && Read(ui.base, controller + 0x1AC) == 13);
  assert(Read(ui.base, controller + 0x370) == 4);
  assert(BoundNode(ui.base, Slot(controller, 12)) == rb);
  assert(BoundNode(ui.base, Slot(controller, 18)) == 0x2000 + 12 * 4);
  assert(std::bit_cast<float>(Read(ui.base, Slot(controller, 12) + 12)) == 180.f);
  assert(Read(ui.base, Slot(controller, 12) + 8) == 118);
  // Every owning binding remains present exactly once for native teardown.
  std::vector<uint32_t> bindings;
  for (unsigned i = 0; i < 20; ++i) bindings.push_back(Read(ui.base, Slot(controller, i)));
  std::sort(bindings.begin(), bindings.end());
  for (unsigned i = 0; i < 20; ++i) assert(bindings[i] == 0x1000 + 0x20 * i);
  const auto compacted = memory;
  assert(UseFourTabs(ui, controller, rb, fourth, sixth) == Result::AlreadyApplied);
  assert(memory == compacted && ui.calls == 1);
  // RB and the divider have different parents. The common ancestor's animated
  // scale must not affect their authored gap.
  constexpr uint32_t tab_parent = 0x2400, common = 0x2600;
  Write(ui.base, rb + 12, common);
  Write(ui.base, fourth + 12, tab_parent);
  Write(ui.base, tab_parent + 12, common);
  auto transform = [&](uint32_t node, uint32_t instance, float x, float scale) {
    Write(ui.base, node + 4, instance);
    Write(ui.base, instance + 4, node);
    Write(ui.base, instance + 0xC, std::bit_cast<uint32_t>(x));
    Write(ui.base, instance + 0x24, std::bit_cast<uint32_t>(scale));
  };
  transform(fourth, 0x500, 162.6327f, 1.f);
  transform(tab_parent, 0x600, 313.f, 1.f);
  transform(common, 0x700, -463.f, .75f);
  Write(ui.base, controller + 0x374, 1);
  Write(ui.base, controller + 0x1E0, std::bit_cast<uint32_t>(83.6327f));
  assert(AnchorRightBumper(ui, controller, rb, fourth));
  assert(std::abs(std::bit_cast<float>(Read(ui.base, Slot(controller, 12) + 12)) - 416.f) < .001f);
  transform(common, 0x700, 100.f, 1.f);
  assert(AnchorRightBumper(ui, controller, rb, fourth));
  assert(std::abs(std::bit_cast<float>(Read(ui.base, Slot(controller, 12) + 12)) - 416.f) < .001f);
  transform(tab_parent, 0x600, 313.f, 0.f);
  assert(!AnchorRightBumper(ui, controller, rb, fourth));
  // Centered static text has no live horzalign property for the stacker to
  // query. Its native relative-background record is the authoritative center.
  std::array<uint32_t, 4> labels{}, backgrounds{};
  std::array<TabMetrics, 4> metrics{};
  for (unsigned n = 0; n <= 13; ++n)
    Write(ui.base, controller + 0x1B0 + n * 4, std::bit_cast<uint32_t>(float(n * 10)));
  for (unsigned i = 0; i < 4; ++i) {
    const auto slot = 3 * i, relative = controller + 0x208 + i * 0x24;
    labels[i] = BoundNode(ui.base, Slot(controller, slot));
    Write(ui.base, Slot(controller, slot) + 4, Read(ui.base, Slot(controller, slot)));
    Write(ui.base, Slot(controller, slot), 0); // Actual text binding arm.
    backgrounds[i] = 0x2800 + i * 0x20;
    Write(ui.base, labels[i] + 12, common);
    Write(ui.base, backgrounds[i] + 12, common);
    Write(ui.base, relative, 0x1800 + i * 0x20);
    Write(ui.base, 0x1804 + i * 0x20, backgrounds[i]);
    Write(ui.base, relative + 4, std::bit_cast<uint32_t>(100.f + 120.f * i));
    Write(ui.base, relative + 8, std::bit_cast<uint32_t>(70.f));
    Write(ui.base, relative + 12, std::bit_cast<uint32_t>(1.f));
    Write(ui.base, relative + 16, slot);
    Write(ui.base, relative + 20, slot + 2);
    Write(ui.base, relative + 24, 0); // Actual imported TabBg mode.
    Write(ui.base, Slot(controller, slot) + 8, std::bit_cast<uint32_t>(50.f));
  }
  forza::GuestPtr(ui.base, controller)[0x378] = 1;
  assert(CenterLabels(ui, controller, labels, backgrounds, metrics));
  assert(forza::GuestPtr(ui.base, controller)[0x378] == 1);
  for (unsigned i = 0; i < 4; ++i) {
    assert(metrics[i].text_x == 115.f + 150.f * i);
    assert(ui.placed_text_x[i] == metrics[i].background_x);
    assert(metrics[i].text_width == 60.f && metrics[i].background_width == 100.f);
    assert(std::bit_cast<float>(Read(ui.base, Slot(controller, 3 * i) + 12)) ==
           115.f + 120.f * i);
  }
  const auto centered = memory;
  assert(CenterLabels(ui, controller, labels, backgrounds, metrics));
  assert(memory == centered); // No accumulated offset on later style passes.
  ui.queried_alignments = {0, 1, 2, 1};
  assert(CenterLabels(ui, controller, labels, backgrounds, metrics, ui.queried_alignments));
  for (unsigned i = 0; i < 4; ++i)
    assert(ui.placed_text_x[i] == metrics[i].background_x);
  memory = centered;
  ui.queried_alignments = {};
  Write(ui.base, controller + 0x220, 1); // Left-anchored UV mode differs.
  const auto unsupported = memory;
  assert(!CenterLabels(ui, controller, labels, backgrounds, metrics));
  assert(memory == unsupported);
  Write(ui.base, controller + 0x220, 2); // Centered UV mode has the same center.
  assert(CenterLabels(ui, controller, labels, backgrounds, metrics));
  assert(metrics[0].text_x == 115.f);
  memory = centered;
  backgrounds[3] = 0;
  assert(!CenterLabels(ui, controller, labels, backgrounds, metrics));
  assert(memory == centered); // Reject incomplete bindings atomically.
  std::puts("Native tab stacker: four-tab compaction, shared-ancestor anchor, ownership and idempotence passed.");
}
