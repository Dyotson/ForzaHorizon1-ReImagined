// Read FH1 TU4's current owned car and its native thumbnail resource.
// tu_address_map: TU4 addresses
#pragma once

#include "guest_addr.h"
#include "guest_memory.h"
#include "fh1_logo_layout.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <bit>
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

// PlayerBar's current-car class/PI path (828C7114..828C71A4). Preserve
// CClassBadge's native image lookup, class color and display-PI conversion.
// This reads the loaded car; it does not estimate an unsaved tune's PI.
template <typename UI>
bool BindCurrentClass(UI& ui,uint32_t controller,uint32_t expected_model,uint64_t* last_value=nullptr) {
#if FORZA_TU4
  if(!controller||ReadWord(ui.base,controller)!=0x8207513C||!expected_model)return false;
  const auto player=ui.Call(0x82915BE0,{});if(!player)return false;
  const auto table=ReadWord(ui.base,player);
  const auto getter=table?ReadWord(ui.base,table+0x78):0;
  const auto car=getter?ui.Call(getter,{player}):0;
  if(!car)return false;
  const auto model=ui.Call(0x826BDCB0,{car});
  if(!model||ReadWord(ui.base,model)!=expected_model)return false;
  const auto car_class=ui.Call(0x825E6518,{car});
  ui.Call(0x825E6620,{car});const double pi=ui.ctx.f1.f64;
  if(!std::isfinite(pi)||pi<=0)return false;
  const uint64_t value=uint64_t(car_class)<<32|std::bit_cast<uint32_t>(float(pi));
  if(last_value&&*last_value==value)return true;
  // A donor has no owning garage datasource. Detach its borrowed pointer via
  // the native setter before explicitly supplying current-car values.
  ui.Call(ReadWord(ui.base,ReadWord(ui.base,controller)+0x64),{controller,0});
  ui.ctx.f1.f64=pi;ui.Call(0x8288F2B0,{controller,0,car_class});
  ui.Call(0x82891220,{controller});
  if(last_value)*last_value=value;
  return true;
#else
  return false;
#endif
}

// The native PlayerBar's localized Data_Car / List_CarMake query (828C71A8).
// Bind separate model and manufacturer fields without converting localized
// guest strings to ASCII. Missing data leaves the target text unchanged.
template <typename UI>
bool BindNames(UI& ui, uint32_t presentation, uint32_t model_node,
               uint32_t manufacturer_node, uint32_t model_id,
               uint32_t logo_material=0, bool* logo_bound=nullptr, bool large_logo=false,
               fh1_logo_layout::Fit* logo_fit=nullptr) {
  if (logo_bound) *logo_bound=false;
  if(logo_fit)*logo_fit={};
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
  const auto sql = std::string("SELECT c.DisplayName AS carName, m.DisplayName AS makeName, m.IconPath AS logoPath "
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
  if (logo_material) {
    const auto key=temporary+16;
    ui.Call(0x82480600,{key,ui.String("logoPath")});
    // Raw filename getter, matching 825B6264. The localized text getter
    // (+4C) used for car names intentionally does not return resource paths.
    const auto filename=ui.Call(ReadWord(ui.base,table+0x44),{result,0,key,0});
    const auto length=filename?ReadWord(ui.base,filename+16):0;
    const auto capacity=filename?ReadWord(ui.base,filename+20):0;
    const auto chars=filename?(capacity<16?filename:ReadWord(ui.base,filename)):0;
    std::string path;
    if (chars && length>0 && length<512 && length<=capacity) for (unsigned i=0;i<length;++i) {
      const auto c=GuestPtr(ui.base,chars+i)[0];
      if (!c) break;
      if (c>127) {path.clear();break;}
      path.push_back(char(c));
    }
    ui.Call(0x82441428,{key,1,0});
    // Fit the artwork inside the larger texture's transparent canvas. Brands
    // without a native LG resource retain their original database image.
    if(large_logo){const auto folder=path.find("Manufacturer_Logos_I");
      if(folder!=std::string::npos)if(const auto fit=fh1_logo_layout::Large(path)){
        path.replace(folder,20,"Manufacturer_Logos_LG");if(logo_fit)*logo_fit=*fit;}}
    // Use the database's logo resource, never the donor's default.
    if (!path.empty() && path.size()<512) {
      const auto binding=temporary+48;
      ui.Call(0x82664A38,{binding});
      WriteWord(ui.base,binding+4,logo_material);
      WriteWord(ui.base,binding+8,presentation);
      ui.Call(0x82F38E80,{binding,ui.String(path),0,8});
      if (logo_bound) *logo_bound=true;
    }
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
