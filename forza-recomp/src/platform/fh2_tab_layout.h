#pragma once

#include "guest_memory.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace forza::fh2_tab_layout {
namespace detail {
inline uint32_t Read(uint8_t* base, uint32_t address) {
  const auto* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}
inline void Write(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = GuestPtr(base, address);
  for (int index = 3; index >= 0; --index) {
    p[index] = uint8_t(value);
    value >>= 8;
  }
}
inline uint32_t Slot(uint32_t stacker, unsigned index) {
  return stacker + 0x6C + 0x10 * index;
}
inline uint32_t BoundNode(uint8_t* base, uint32_t slot) {
  // A slot owns either a generic element binding or a text element binding.
  auto binding = Read(base, slot);
  if (!binding) binding = Read(base, slot + 4);
  return binding ? Read(base, binding + 4) : 0;
}
}  // namespace detail

enum class Result { NotReady, AlreadyApplied, Applied };

struct TabMetrics {
  float text_x = 0;
  float background_x = 0;
  float text_width = 0;
  float background_width = 0;
};

// Static FBF text can be centered without exposing a live horzalign property.
// SUPER_STACKER's optional alignment query then reports left alignment and
// misses half the changed text width. Use its own relative-background records
// to establish centered authored anchors, preserving native measured layout.
template<class Native>
bool CenterLabels(Native& ui, uint32_t stacker,
                  const std::array<uint32_t, 4>& labels,
                  const std::array<uint32_t, 4>& backgrounds,
                  std::array<TabMetrics, 4>& metrics,
                  const std::array<uint32_t, 4>& native_alignments = {}) {
  using namespace detail;
  if (!stacker || Read(ui.base, stacker) != 0x820814AC ||
      Read(ui.base, stacker + 0x1AC) != 13 ||
      Read(ui.base, stacker + 0x370) != 4 ||
      Read(ui.base, stacker + 0x374) != 1) return false;
  std::array<float, 4> anchors{};
  const auto scalar = [&](uint32_t at) {
    return std::bit_cast<float>(Read(ui.base, at));
  };
  const auto delta = [&](unsigned slot) { return scalar(stacker + 0x1B0 + slot * 4); };
  for (unsigned i = 0; i < 4; ++i) {
    const auto slot = 3 * i;
    const auto relative = stacker + 0x208 + 0x24 * i;
    const auto binding = Read(ui.base, relative);
    const auto mode = Read(ui.base, relative + 0x18);
    if (!labels[i] || !backgrounds[i] || !binding ||
        BoundNode(ui.base, Slot(stacker, slot)) != labels[i] ||
        Read(ui.base, binding + 4) != backgrounds[i] ||
        Read(ui.base, labels[i] + 12) != Read(ui.base, backgrounds[i] + 12) ||
        Read(ui.base, relative + 0x10) != slot ||
        Read(ui.base, relative + 0x14) != slot + 2 ||
        (mode != 0 && mode != 2)) return false;
    // Actual TabBg uses mode 0: 828CFE54..84 sets the center and scale.
    // Mode 2 uses the same center/scale at 828CFD38..78, then adjusts image UV.
    // Mode 1 instead anchors at the left edge and must not use this formula.
    const auto span = delta(slot + 3) - delta(slot);
    const auto background_x = scalar(relative + 4) + delta(slot) + .5f * span;
    const auto text_width = delta(slot + 1) - delta(slot) +
                            scalar(Slot(stacker, slot) + 8);
    const auto background_width = span + scalar(relative + 8) * scalar(relative + 12);
    const auto alignment_delta = delta(slot + 1) - delta(slot);
    const float alignment_adjustment = native_alignments[i] == 1 ? .5f * alignment_delta :
        native_alignments[i] == 2 ? alignment_delta : 0.f;
    anchors[i] = background_x - delta(slot) - alignment_adjustment;
    if (!std::isfinite(anchors[i]) || !std::isfinite(text_width) ||
        !std::isfinite(background_width) || text_width <= 0 || background_width <= 0)
      return false;
    metrics[i] = {background_x, background_x, text_width, background_width};
  }
  // This flag gates the entire text-binding setter, not just alignment. Keep
  // it enabled; the authored anchor above compensates its queried adjustment.
  // Native Place still owns all positioning and background resizing.
  GuestPtr(ui.base, stacker)[0x378] = 1;
  for (unsigned i = 0; i < 4; ++i)
    Write(ui.base, Slot(stacker, 3 * i) + 12, std::bit_cast<uint32_t>(anchors[i]));
  ui.Call(0x828CFAB8, {stacker});
  return true;
}

// Convert an X coordinate using transform parameters, stopping before the
// shared ancestor's intro animation. Positions/scales are the native fields
// populated by 82FE3B80/82FE3BF8. This horizontal tab bar has no local rotation.
inline bool ToAncestor(uint8_t* base, uint32_t node, uint32_t ancestor,
                       float& scale, float& offset) {
  scale = 1;
  offset = 0;
  for (unsigned depth = 0; node && depth < 64; ++depth) {
    if (node == ancestor) return true;
    const auto instance = detail::Read(base, node + 4);
    if (!instance || detail::Read(base, instance + 4) != node) return false;
    auto value = [&](uint32_t field) {
      return std::bit_cast<float>(detail::Read(base, instance + field));
    };
    const auto x = value(0x0C), sx = value(0x24);
    if (!std::isfinite(x) || !std::isfinite(sx) || std::abs(sx) < 0.0001f ||
        value(0x18) != 0 || value(0x1C) != 0 || value(0x20) != 0)
      return false;
    offset = offset * sx + x;
    scale *= sx;
    node = detail::Read(base, node + 12);
  }
  return false;
}

// The divider belongs to Tab_4, while RB belongs to the outer tab control.
// Their authored X values cannot be subtracted directly. Apply after text has
// settled; the native stacker then preserves this anchor on subsequent frames.
template<class Native>
bool AnchorRightBumper(Native& ui, uint32_t stacker, uint32_t rb_node,
                      uint32_t divider, float gap = 24.f) {
  using namespace detail;
  if (!stacker || !rb_node || !divider || !std::isfinite(gap) ||
      Read(ui.base, stacker) != 0x820814AC ||
      Read(ui.base, stacker + 0x1AC) != 13 ||
      Read(ui.base, stacker + 0x374) != 1 ||
      BoundNode(ui.base, Slot(stacker, 12)) != rb_node ||
      BoundNode(ui.base, Slot(stacker, 11)) != divider)
    return false;
  const auto rb_parent = Read(ui.base, rb_node + 12);
  uint32_t ancestor = divider;
  bool found = false;
  for (unsigned depth = 0; ancestor && depth < 64 && !found; ++depth) {
    auto parent = rb_parent;
    for (unsigned steps = 0; parent && steps < 64; ++steps) {
      if (parent == ancestor) { found = true; break; }
      parent = Read(ui.base, parent + 12);
    }
    if (!found) ancestor = Read(ui.base, ancestor + 12);
  }
  if (!found) return false;
  float divider_scale, divider_x, parent_scale, parent_x;
  if (!ToAncestor(ui.base, divider, ancestor, divider_scale, divider_x) ||
      !ToAncestor(ui.base, rb_parent, ancestor, parent_scale, parent_x))
    return false;
  const auto delta = std::bit_cast<float>(Read(ui.base, stacker + 0x1E0));
  // The RB component's authored left edge is 24 units before its origin.
  // A 24-unit anchor therefore joins its panel to the last tab, matching the
  // LB panel's zero exterior gap. The old 36 left a right-only 12-unit gutter.
  const float authored_x = (divider_x - parent_x) / parent_scale + gap - delta;
  if (!std::isfinite(authored_x)) return false;
  Write(ui.base, Slot(stacker, 12) + 12, std::bit_cast<uint32_t>(authored_x));
  ui.Call(0x828D1E08, {stacker});
  return true;
}

// FH1 TU4 SUPER_STACKER, created by 826EE450/828DF810. Call after its init
// (828CF6A8) has populated the bindings and after setting the four tab labels.
// Capture its native controller from the factory return, not the UI wrapper.
// Required nodes are under ANTH_TabControl_New: Component_RB and the two
// GROUP_ITEMS.Tab_4/Tab_6.TabDivider nodes. These identify the top-level stacker
// unambiguously among the scene's notification stackers.
template<class Native>
Result UseFourTabs(Native& ui, uint32_t stacker, uint32_t rb_node,
                   uint32_t fourth_divider, uint32_t sixth_divider) {
  using namespace detail;
  if (!stacker || !rb_node || !fourth_divider || !sixth_divider ||
      Read(ui.base, stacker) != 0x820814AC)
    return Result::NotReady;
  const auto count = Read(ui.base, stacker + 0x1AC);
  if (count == 13 && BoundNode(ui.base, Slot(stacker, 12)) == rb_node)
    return Result::AlreadyApplied;
  if (count < 19 || count > 20 ||
      BoundNode(ui.base, Slot(stacker, 18)) != rb_node ||
      BoundNode(ui.base, Slot(stacker, 11)) != fourth_divider ||
      BoundNode(ui.base, Slot(stacker, 17)) != sixth_divider)
    return Result::NotReady;

  const auto relative_count = Read(ui.base, stacker + 0x370);
  if (relative_count > 10) return Result::NotReady;
  const auto rb_x = std::bit_cast<float>(Read(ui.base, Slot(stacker, 18) + 12));
  if (!std::isfinite(rb_x)) return Result::NotReady;

  // Move RB immediately after four (text, notification, divider) triples.
  // Swap entire owning records rather than copy their binding pointers:
  // destructor 828DF8E8 always destroys all 20 records via 828CA7F8.
  const auto destination = Slot(stacker, 12), source = Slot(stacker, 18);
  std::array<uint32_t, 4> saved{};
  for (unsigned i = 0; i < saved.size(); ++i)
    saved[i] = Read(ui.base, destination + i * 4);
  for (unsigned i = 0; i < saved.size(); ++i)
    Write(ui.base, destination + i * 4, Read(ui.base, source + i * 4));
  for (unsigned i = 0; i < saved.size(); ++i)
    Write(ui.base, source + i * 4, saved[i]);
  Write(ui.base, stacker + 0x1AC, 13);
  // The first four relative entries size Tab_1..Tab_4 backgrounds. Their source
  // indices remain unchanged; hidden Tab_5/Tab_6 backgrounds no longer update.
  Write(ui.base, stacker + 0x370, std::min(relative_count, 4u));

  // The original update measures current text widths and applies native X
  // properties. It continues doing this normally; no per-frame correction or
  // direct transform mutation is installed by this helper.
  ui.Call(0x828D1E08, {stacker});
  return Result::Applied;
}

}  // namespace forza::fh2_tab_layout
