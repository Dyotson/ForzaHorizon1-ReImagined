#include "../src/platform/fh2_cars_layout.h"
#include <cassert>
#include <cstdio>
#include <map>

struct Native {
  uint8_t* base;
  std::map<std::string, uint32_t> nodes;
  unsigned transforms = 0;
  unsigned opacity_changes = 0;
  std::map<uint32_t, bool> visibility;
  uint32_t Node(const std::string& path) { return nodes[path]; }
  uint32_t Read(uint32_t at) const {
    return uint32_t(base[at]) << 24 | uint32_t(base[at + 1]) << 16 |
           uint32_t(base[at + 2]) << 8 | base[at + 3];
  }
  void Write(uint32_t at, uint32_t value) {
    for (int i = 3; i >= 0; --i) { base[at + i] = uint8_t(value); value >>= 8; }
  }
  void PositionIn(uint32_t presentation, uint32_t node, float x, float y) {
    assert(presentation == 0xB000);
    ++transforms;
    Write(Read(node + 4) + 0xC, std::bit_cast<uint32_t>(x));
    Write(Read(node + 4) + 0x10, std::bit_cast<uint32_t>(y));
  }
  void ScaleIn(uint32_t presentation, uint32_t node, float x, float y) {
    assert(presentation == 0xB000);
    ++transforms;
    Write(Read(node + 4) + 0x24, std::bit_cast<uint32_t>(x));
    Write(Read(node + 4) + 0x28, std::bit_cast<uint32_t>(y));
  }
  void ShowIn(uint32_t presentation, uint32_t node, bool show) {
    assert(presentation == 0xB000);
    visibility[node] = show;
  }
  void OpacityIn(uint32_t presentation, uint32_t node, float opacity) {
    assert(presentation == 0xB000);
    ++opacity_changes;
    Write(Read(node + 4) + 0xE0, std::bit_cast<uint32_t>(opacity / 100.f));
  }
};

int main() {
  std::array<uint8_t, 0x4000> memory{};
  Native ui{memory.data()};
  constexpr const char* paths[]{"0", "0.ButtonStates.Images.White_BG", "1",
      "1.White_BG", "2", "2.White_BG", "3", "3.Group",
      "3.Group.White_BG", "3.Infographics_Set1"};
  uint32_t address = 0x100;
  for (const auto* path : paths) {
    ui.nodes[path] = address;
    ui.Write(address + 4, address + 0x80);
    ui.Write(address + 0x84, address);
    address += 0x200;
  }
  const auto paper = ui.nodes["3.Group.White_BG"];
  const auto infographic = ui.nodes["3.Infographics_Set1"];
  ui.Write(ui.Read(infographic + 4) + 0xE0, std::bit_cast<uint32_t>(1.f));
  ui.nodes["3.Group.White_BG"] = 0;
  const auto original = memory;
  assert(!forza::fh2_cars_layout::Apply(ui, 0xB000, ""));
  assert(memory == original && ui.transforms == 0 && ui.visibility.empty());
  ui.nodes["3.Group.White_BG"] = paper;
  assert(forza::fh2_cars_layout::Apply(ui, 0xB000, ""));
  assert(!ui.visibility[ui.nodes["3.Infographics_Set1"]]);
  assert(ui.visibility[ui.nodes["3.Group"]]);
  assert(ui.visibility[ui.nodes["3"]]);
  assert(ui.opacity_changes == 2);
  const auto applied = memory;
  const auto transforms = ui.transforms;
  assert(forza::fh2_cars_layout::Apply(ui, 0xB000, ""));
  assert(memory == applied && ui.transforms == transforms);
  assert(ui.opacity_changes == 2);
  // A subsequent native slide can restore authored geometry. Reapply exactly
  // once to repair it without compounding scales or touching other nodes.
  ui.Write(ui.Read(paper + 4) + 0x24, std::bit_cast<uint32_t>(1.83f));
  assert(forza::fh2_cars_layout::Apply(ui, 0xB000, ""));
  assert(memory == applied && ui.transforms == transforms + 1);
  ui.Write(ui.Read(ui.nodes["3.Group"] + 4) + 0xE0, 0);
  assert(forza::fh2_cars_layout::Apply(ui, 0xB000, ""));
  assert(memory == applied && ui.opacity_changes == 3);
  std::puts("Cars layout: complete bindings, presentation scope and idempotent slide repair passed.");
}
