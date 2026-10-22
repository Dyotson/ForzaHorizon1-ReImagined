#pragma once

#include "guest_memory.h"
#include <array>
#include <bit>
#include <cstdint>
#include <string>

namespace forza::fh2_cars_layout {

// Reuse Grid 03's existing component ownership and 100-by-100 paper meshes.
// Resize paper leaves only: scaling a button parent would also stretch fonts.
// Label styles and logical action-to-button mapping belong to the caller.
template<class Native>
bool Apply(Native& ui, uint32_t presentation,
           const std::string& prefix =
               "parent.Layer.Grid_Menu.Group.Grid_Menu_03.Group.Button_") {
  if (!presentation) return false;
  const auto read = [&](uint32_t at) {
    const auto* p = GuestPtr(ui.base, at);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
           uint32_t(p[2]) << 8 | p[3];
  };
  struct Placement {
    uint32_t node;
    float x, y, scale_x, scale_y;
  };
  const std::array<Placement, 9> placements{{
      {ui.Node(prefix + "0"), -185.f, 1.f, 1.f, 1.f},
      {ui.Node(prefix + "0.ButtonStates.Images.White_BG"), 0.f, 0.f, 5.5f, 3.7f},
      {ui.Node(prefix + "1"), 280.f, 122.f, 1.f, 1.f},
      {ui.Node(prefix + "1.White_BG"), 0.f, 0.f, 3.6f, 1.12f},
      {ui.Node(prefix + "2"), 280.f, 0.f, 1.f, 1.f},
      {ui.Node(prefix + "2.White_BG"), 0.f, 0.f, 3.6f, 1.12f},
      {ui.Node(prefix + "3"), 280.f, -122.f, 1.f, 1.f},
      {ui.Node(prefix + "3.Group"), 0.f, 0.f, 1.f, 1.f},
      {ui.Node(prefix + "3.Group.White_BG"), 0.f, 0.f, 3.6f, 1.12f},
  }};
  const auto infographics = ui.Node(prefix + "3.Infographics_Set1");
  if (!infographics) return false;
  const auto infographic_instance = read(infographics + 4);
  if (!infographic_instance || read(infographic_instance + 4) != infographics)
    return false;
  // Reject an incomplete imported layout before changing any node.
  for (const auto& value : placements) {
    if (!value.node) return false;
    const auto instance = read(value.node + 4);
    if (!instance || read(instance + 4) != value.node) return false;
  }
  for (const auto& value : placements) {
    const auto instance = read(value.node + 4);
    if (read(instance + 0xC) != std::bit_cast<uint32_t>(value.x) ||
        read(instance + 0x10) != std::bit_cast<uint32_t>(value.y))
      ui.PositionIn(presentation, value.node, value.x, value.y);
    if (read(instance + 0x24) != std::bit_cast<uint32_t>(value.scale_x) ||
        read(instance + 0x28) != std::bit_cast<uint32_t>(value.scale_y))
      ui.ScaleIn(presentation, value.node, value.scale_x, value.scale_y);
  }
  // Button 3 owns alternate infographic slides. Visibility bits and opacity
  // are separate native properties: activating a group cannot repair an
  // authored slide that left its alpha at zero. This helper is Cars-only.
  ui.ShowIn(presentation, placements[6].node, true);
  ui.ShowIn(presentation, placements[7].node, true);
  ui.ShowIn(presentation, infographics, false);
  const auto group_instance = read(placements[7].node + 4);
  if (read(group_instance + 0xE0) != std::bit_cast<uint32_t>(1.f))
    ui.OpacityIn(presentation, placements[7].node, 100.f);
  if (read(infographic_instance + 0xE0) != std::bit_cast<uint32_t>(0.f))
    ui.OpacityIn(presentation, infographics, 0.f);
  return true;
}

}  // namespace forza::fh2_cars_layout
