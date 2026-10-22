// Read FH1 TU4's current owned car and its native thumbnail resource.
// tu_address_map: TU4 addresses
#pragma once

#include "guest_addr.h"
#include "guest_memory.h"
#include <cstdint>
#include <cstring>
#include <string>

namespace forza::fh1_current_car {

struct CurrentCar {
  uint32_t owned_id = 0;
  uint32_t model_id = 0;
  std::string thumbnail;
  bool owned_thumbnail = false;
  explicit operator bool() const { return model_id != 0; }
};

inline uint32_t ReadWord(uint8_t* base, uint32_t address) {
  const auto* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}

inline void WriteWord(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = GuestPtr(base, address);
  for (unsigned i = 0; i < 4; ++i)
    p[i] = uint8_t(value >> (24 - i * 8));
}

// The native PlayerBar's localized Data_Car / List_CarMake query (828C71A8).
// Bind separate model and manufacturer fields without converting localized
// guest strings to ASCII. Missing data leaves the target text unchanged.
template <typename UI>
bool BindNames(UI& ui, uint32_t presentation, uint32_t model_node,
               uint32_t manufacturer_node, uint32_t model_id) {
#if FORZA_TU4
  if (!presentation || !model_id || (!model_node && !manufacturer_node))
    return false;
  const auto services = ReadWord(ui.base, 0x833CAAE4);
  if (!services) return false;
  const auto database_reference = ui.Call(0x82F1E918, {services});
  const auto database = database_reference ? ReadWord(ui.base, database_reference) : 0;
  if (!database) return false;
  const auto temporary = ui.Call(0x824265C8, {80});
  if (!temporary) return false;
  std::memset(GuestPtr(ui.base, temporary), 0, 80);
  struct Cleanup {
    UI& ui;
    uint32_t memory;
    ~Cleanup() {
      ui.Call(0x824CEAB0, {memory + 4});
      ui.Call(0x824CEAB0, {memory});
      ui.Call(0x8242D1A0, {memory});
    }
  } cleanup{ui, temporary};
  const auto sql = std::string("SELECT c.DisplayName AS carName, m.DisplayName AS makeName "
                               "FROM Data_Car c INNER JOIN List_CarMake m ON c.MakeId=m.ID "
                               "WHERE c.Id = ") + std::to_string(model_id);
  ui.Call(ReadWord(ui.base, ReadWord(ui.base, database) + 0x20),
          {temporary, database, ui.String(sql)});
  const auto result = ReadWord(ui.base, temporary);
  if (!result) return false;
  const auto table = ReadWord(ui.base, result);
  if (!ui.Call(ReadWord(ui.base, table + 0xB8), {result})) return false;
  bool bound = false;
  const uint32_t nodes[] = {model_node, manufacturer_node};
  const char* columns[] = {"carName", "makeName"};
  for (unsigned i = 0; i < 2; ++i) {
    if (!nodes[i]) continue;
    const auto key = temporary + 16;
    ui.Call(0x82480600, {key, ui.String(columns[i])});
    ui.Call(ReadWord(ui.base, table + 0x4C),
            {temporary + 4, result, 0, key, 0});
    ui.Call(0x82441428, {key, 1, 0});
    if (ReadWord(ui.base, temporary + 4)) {
      const auto text = temporary + 48;
      WriteWord(ui.base, text, 0x820307A4);
      WriteWord(ui.base, text + 4, nodes[i]);
      WriteWord(ui.base, text + 8, presentation);
      ui.Call(0x82F3A728, {text, temporary + 4});
      bound = true;
    }
    ui.Call(0x824CEAB0, {temporary + 4});
  }
  return bound;
#else
  return false;
#endif
}

// Bind the initialized native CAR_STATS controller to the owned car's actual
// upgrade/performance row. This is the CMyCars v100 / CCarSelect update path
// (827144E8 and 8275D6C8), without creating a second garage screen.
template <typename UI>
bool BindPerformance(UI& ui, uint32_t controller, uint32_t owned_id) {
#if FORZA_TU4
  if (!controller || ReadWord(ui.base, controller) != 0x82074F4C)
    return false;
  const auto bars = ReadWord(ui.base, controller + 0x74);
  const auto bars_end = ReadWord(ui.base, controller + 0x78);
  if (!bars || bars_end < bars || bars_end - bars < 5 * sizeof(uint32_t))
    return false;
  for (unsigned i = 0; i < 5; ++i)
    if (!ReadWord(ui.base, bars + i * sizeof(uint32_t))) return false;
  const auto player = ui.Call(0x82915BE0, {});
  if (!player) return false;
  const auto vtable = ReadWord(ui.base, player);
  const auto getter = vtable ? ReadWord(ui.base, vtable + 0x64) : 0;
  const auto garage = getter ? ui.Call(getter, {player}) : 0;
  if (!garage) return false;
  const auto allocation = ui.Call(0x824265C8, {16});
  if (!allocation) return false;
  const auto model = ui.Call(0x826F85B8, {allocation, garage, owned_id});
  // Constructor starts with zero references. Native assignment adds exactly
  // one reference and releases the prior model; the controller owns it now.
  ui.Call(0x82E180E0, {controller + 0x70, model});
  // Mode 0 (828905D0) shows native specifications: power, torque, weight,
  // front weight distribution and displacement, with native unit formatting.
  std::memset(GuestPtr(ui.base, controller + 0x84), 0, sizeof(uint32_t));
  ui.Call(0x828928B0, {controller});
  return true;
#else
  return false;
#endif
}

// UI supplies base, Call(address, arguments), and String(host string). The
// native garage retains its current car; our temporary reference and native
// string are released before returning host-owned data. No thumbnail render,
// save, garage selection or world state change is requested.
template <typename UI>
CurrentCar ReadCurrentCar(UI& ui) {
  CurrentCar result;
#if FORZA_TU4
  const auto player = ui.Call(0x82915BE0, {}); // USER0 facade v28
  if (!player) return result;
  const auto player_vtable = ReadWord(ui.base, player);
  if (!player_vtable) return result;
  const auto garage_getter = ReadWord(ui.base, player_vtable + 0x64);
  if (!garage_getter) return result;
  const auto garage = ui.Call(garage_getter, {player});
  if (!garage) return result;

  const auto temporary = ui.Call(0x824265C8, {64});
  if (!temporary) return result;
  std::memset(GuestPtr(ui.base, temporary), 0, 64);
  const auto car_reference = temporary;
  const auto path = temporary + 16;
  ui.Call(0x82480600, {path, ui.String("")});
  struct Cleanup {
    UI& ui;
    uint32_t memory, reference, string;
    ~Cleanup() {
      ui.Call(0x82441428, {string, 1, 0});
      ui.Call(0x824CEAB0, {reference});
      ui.Call(0x8242D1A0, {memory});
    }
  } cleanup{ui, temporary, car_reference, path};

  // 825A3250 returns garage's current full car (or lazily reads its existing
  // CurrentCar row); the same accessor is used by native garage/player menus.
  ui.Call(0x825A3250, {car_reference, garage});
  const auto car = ReadWord(ui.base, car_reference);
  if (!car) return result;
  result.model_id = ReadWord(ui.base, car + 0x38);
  result.owned_id = ReadWord(ui.base, car + 0x28C);
  if (!result.model_id) return result;

  // Owned garage image first; the original Data_Car image is the fallback.
  // Deliberately avoid 825B63B8, which may generate and persist thumbnails.
  if (ReadWord(ui.base, car + 0x898)) {
    ui.Call(0x825B62A0, {path, car});
    result.owned_thumbnail = ReadWord(ui.base, path + 0x10) != 0;
  }
  if (!result.owned_thumbnail) {
    ui.Call(0x82441428, {path, 0, 0});
    ui.Call(0x825B61D8, {path, car});
  }
  const auto length = ReadWord(ui.base, path + 0x10);
  const auto capacity = ReadWord(ui.base, path + 0x14);
  const auto characters = capacity < 16 ? path : ReadWord(ui.base, path);
  if (characters && length > 0 && length <= 4096 && length <= capacity)
    result.thumbnail.assign(reinterpret_cast<const char*>(GuestPtr(ui.base, characters)), length);
#endif
  return result;
}

} // namespace forza::fh1_current_car
