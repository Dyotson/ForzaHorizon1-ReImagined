#pragma once

#include "fh2_grid_events.h"
#include "guest_memory.h"
#include <cstdint>
#include <span>

namespace forza::fh2_focus_binding {

// Supply only factory-captured button contracts from the current scene engine.
// Raw UI nodes have a parent at +0x0C even when their record is only 0x20 bytes.
// The nearest contract below the requested button root wins; traversal is
// bounded so a corrupt parent cycle cannot hang the UI thread.
template<class ParentOf>
uint32_t FindContract(uint32_t button_root,
                      std::span<const uint32_t> button_contracts,
                      ParentOf parent_of) {
  if (!button_root) return 0;
  uint32_t result = 0;
  unsigned best_depth = 65;
  for (const auto contract : button_contracts) {
    auto ancestor = contract;
    for (unsigned depth = 0; ancestor && depth < 64; ++depth) {
      if (ancestor == button_root) {
        if (depth < best_depth) {
          result = contract;
          best_depth = depth;
        }
        break;
      }
      ancestor = parent_of(ancestor);
    }
  }
  return result;
}

class Focus {
public:
  uint32_t current() const { return current_; }

  // Emit takes (captured contract node, native event hash). Event wrappers and
  // their lifetime belong to the caller; no FH2 object layout enters FH1.
  template<class Emit>
  void Set(uint32_t contract, Emit emit, bool immediate = true) {
    if (contract == current_) return;
    if (current_) emit(current_, fh2_grid_events::Blur(immediate));
    current_ = contract;
    if (current_) emit(current_, fh2_grid_events::Focus(immediate));
  }

  template<class ParentOf, class Emit>
  bool Bind(uint32_t button_root, std::span<const uint32_t> button_contracts,
            ParentOf parent_of, Emit emit, bool immediate = true) {
    const auto contract = FindContract(button_root, button_contracts, parent_of);
    if (!contract) return false;
    Set(contract, emit, immediate);
    return true;
  }

  // On scene teardown the old nodes no longer accept events.
  void Forget() { current_ = 0; }

private:
  uint32_t current_ = 0;
};

// Verified FH1 TU4 material setter: 82F37C40 unpacks 0xRRGGBB to float
// channels in 0..255, then 82F375E8 updates diffuse.r/g/b and dirties the
// render object. Pass the actual material node and 12 guest scratch bytes.
template<class Native>
bool SetMaterialRgb(Native& ui, uint32_t material_node, uint32_t engine,
                    uint32_t scratch, uint32_t rgb) {
  if (!material_node || !engine || !scratch) return false;
  auto write = [&](uint32_t address, uint32_t value) {
    auto* bytes = GuestPtr(ui.base, address);
    for (int index = 3; index >= 0; --index) {
      bytes[index] = uint8_t(value);
      value >>= 8;
    }
  };
  write(scratch, 0x820307A4);
  write(scratch + 4, material_node);
  write(scratch + 8, engine);
  ui.Call(0x82F37C40, {scratch, rgb & 0xFFFFFF});
  return true;
}

}  // namespace forza::fh2_focus_binding
