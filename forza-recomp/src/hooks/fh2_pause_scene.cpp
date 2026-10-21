// FH2's original scene and page events, bound to native FH1 TU4 actions.
// tu_address_map: TU4 addresses
#include "../platform/fh2_pause_scene.h"
#include "../platform/guest_addr.h"
#if FORZA_TU4 && defined(FORZA_FH2_PAUSE_SCENE)
#include "../platform/fh1_pause_actions.h"
#include "../platform/fh2_grid_events.h"
#include "../platform/fh2_focus_binding.h"
#include "../platform/fh2_tab_layout.h"
#include "../platform/fh2_navigation_adapter.h"
#include "../platform/guest_memory.h"
#include "../platform/offline_cars.h"
#include "../platform/fh1_festival_travel.h"
#include "../platform/fh1_car_recovery.h"
#include "../kernel/live.h"
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

extern "C" REX_FUNC(__imp__sub_82F39B08);
extern "C" REX_FUNC(__imp__sub_827DFB18);
extern "C" REX_FUNC(__imp__sub_827D1578);
extern "C" REX_FUNC(__imp__sub_827DAB28);
extern "C" REX_FUNC(__imp__sub_827DE0D0);
extern "C" REX_FUNC(__imp__XamInputGetState);
extern "C" REX_FUNC(__imp__XamUserGetName);
extern "C" REX_FUNC(__imp__sub_828BAF48);

namespace forza::fh2_pause_scene {
namespace {
uint32_t Read(uint8_t* base, uint32_t address) {
  const auto* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}
void Write(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = GuestPtr(base, address);
  for (int i = 3; i >= 0; --i) {
    p[i] = uint8_t(value);
    value >>= 8;
  }
}
uint32_t Hash(const std::string& text) {
  uint32_t hash = 0;
  for (const auto character : text) hash = uint8_t(character) + hash * 65599;
  return hash * 65599;
}
struct Contract {
  uint32_t node = 0;
  uint32_t parent = 0;
  std::string type;
};
std::mutex contracts_mutex;
std::map<uint32_t, std::vector<Contract>> contracts;
std::map<uint32_t,std::vector<uint32_t>> stackers;
std::map<uint32_t,std::vector<uint32_t>> class_badges;
std::map<uint32_t, uint32_t> event_wrappers;
struct ProgressController {
  uint32_t node = 0;
  uint32_t controller = 0;
};
std::map<uint32_t, std::vector<ProgressController>> progress_controllers;
struct FestivalGauge {
  uint32_t root = 0;
  uint32_t controller = 0;
  uint32_t number = 0;
  uint32_t percent = 0;
  float fraction = 0;
};
FestivalGauge festival_gauge;
struct State {
  uint32_t screen = 0;
  uint32_t engine = 0;
  uint32_t access = 0;
  uint32_t page = 0;
  uint32_t selection = 0;
  uint32_t grid = 0;
  uint32_t tab_stacker = 0;
  uint32_t right_bumper = 0;
  uint32_t last_divider = 0;
  uint16_t previous = 0;
  uint16_t blocked = 0;
  bool visible = true;
  uint32_t layout = 0;
  bool garage_available = false;
  bool autoshow_available = false;
  bool recovery_available = false;
  bool footer_ready = false;
  unsigned footer_refresh_frames = 0;
  unsigned restore_focus_ticks = 0;
};
State state;
struct GarageHeader {
  uint32_t preloader = 0;
  uint32_t presentation = 0;
  uint32_t root = 0;
  uint32_t player_bar = 0;
  uint32_t credits = 0;
  uint32_t username = 0;
  uint32_t username_paper = 0;
  std::vector<uint32_t> username_ancestry;
  std::vector<uint32_t> username_hidden;
  std::string username_value;
  bool username_styled = false;
  unsigned username_fit_frames = 0;
  uint32_t controller = 0;
  uint32_t render_root = 0;
  uint32_t render_owner = 0;
  uint32_t render_runtime = 0;
  std::vector<uint32_t> hidden;
  unsigned frames = 0;
  bool ready = false;
  bool visible = false;
};
GarageHeader garage_header;
struct PlayerIdentity {
  uint32_t bar = 0;
  uint32_t name = 0;
  uint32_t status = 0;
  std::vector<uint32_t> visible;
  std::vector<uint32_t> hidden;
  std::string gamertag;
  std::string connection;
  unsigned frames = 0;
};
PlayerIdentity player_identity;
std::optional<uint32_t> return_page;
std::optional<uint32_t> return_selection;
unsigned theme_frames = 0;
struct TileLabelStyle {
  uint32_t node;
  float x, y, width;
  bool title;
};
std::vector<TileLabelStyle> tile_label_styles;
std::vector<uint32_t> tile_underlines;
struct TileTransform {
  float x, y, scale_x, scale_y;
};
struct HomeTileGeometry {
  uint32_t node;
  TileTransform original;
  TileTransform tall;
};
std::vector<HomeTileGeometry> home_tile_geometry;
std::vector<uint32_t> theme_titles;
std::vector<uint32_t> theme_descriptions;
std::vector<std::pair<uint32_t, uint32_t>> theme_backgrounds;
std::vector<uint32_t> theme_accents;
std::vector<uint32_t> theme_panels;
std::vector<uint32_t> theme_hidden;
std::vector<std::pair<uint32_t, uint32_t>> theme_tabs;
std::vector<uint32_t> festival_progress_nodes;
std::string festival_progress_text;
std::vector<uint32_t> festival_next_nodes;
unsigned festival_progress_frames = 0;

class Native {
public:
  PPCContext ctx;
  uint8_t* base;
  uint32_t scratch;

  Native(const PPCContext& context, uint8_t* memory)
      : ctx(context), base(memory),
        scratch(REX_KERNEL_MEMORY()->SystemHeapAlloc(1024)) {}
  ~Native() {
    if (scratch) REX_KERNEL_MEMORY()->SystemHeapFree(scratch);
  }
  uint32_t Call(uint32_t address, std::initializer_list<uint32_t> args) {
    unsigned index = 0;
    for (const auto value : args) switch (index++) {
      case 0: ctx.r3.u64 = value; break;
      case 1: ctx.r4.u64 = value; break;
      case 2: ctx.r5.u64 = value; break;
      case 3: ctx.r6.u64 = value; break;
      case 4: ctx.r7.u64 = value; break;
      case 5: ctx.r8.u64 = value; break;
    }
    rex::runtime::ResolveIndirectFunction(address)(ctx, base);
    return ctx.r3.u32;
  }
  uint32_t String(const std::string& text) {
    if (text.size() > 240) return 0;
    std::copy(text.begin(), text.end(), GuestPtr(base, scratch + 512));
    GuestPtr(base, scratch + 512)[text.size()] = 0;
    return scratch + 512;
  }
  uint32_t Node(const std::string& path) {
    return Call(0x82F348B8,
                {state.access, Read(base, state.screen + 0x58), String(path), 0});
  }
  uint32_t TextRef(const std::string& text) {
    if (text.size() > 240) return 0;
    auto* output = GuestPtr(base, scratch + 32);
    for (size_t i = 0; i < text.size(); ++i) {
      output[i * 2] = 0;
      output[i * 2 + 1] = uint8_t(text[i]);
    }
    output[text.size() * 2] = output[text.size() * 2 + 1] = 0;
    const auto object = Call(0x824265C8, {16});
    if (!object) return 0;
    Write(base, object, 0x82001674);
    for (unsigned offset = 4; offset < 16; offset += 4)
      Write(base, object + offset, 0);
    Call(0x82D6DE20, {object, scratch + 32, uint32_t(text.size())});
    Call(0x82555B90, {scratch, object});
    return scratch;
  }
  void TextIn(uint32_t presentation, uint32_t node, const std::string& text) {
    if (!node || !TextRef(text)) return;
    Write(base, scratch + 768, 0x820307A4);
    Write(base, scratch + 772, node);
    Write(base, scratch + 776, presentation);
    Call(0x82F3A728, {scratch + 768, scratch});
    Call(0x824CEAB0, {scratch});
  }
  void Text(uint32_t node, const std::string& text) {
    TextIn(state.engine, node, text);
  }
  void OpacityIn(uint32_t presentation, uint32_t node, float value) {
    if (!node) return;
    const auto hash = Call(0x82F354E0, {String("opacity")});
    if (!Call(0x82FEF130, {node, hash})) {
      // Some imported static nodes have no authored opacity property. Match
      // the native instance cache updated by 82FE3B64, only for an instance
      // whose back-reference confirms this node owns the cache.
      const auto instance = Read(base, node + 4);
      if (instance && Read(base, instance + 4) == node) {
        Write(base, instance + 0xE0,
              std::bit_cast<uint32_t>(std::clamp(value / 100.f, 0.f, 1.f)));
        GuestPtr(base, instance + 0xFB)[0] |= 0x80;
        Write(base, scratch + 800, node);
        const auto dirty = Call(Read(base, Read(base, presentation) + 0x7C),
                                {presentation});
        if (dirty) Call(0x82FE57F0, {dirty, scratch + 800});
      }
      return;
    }
    ctx.f1.f64 = value;
    Call(0x82F34DE0, {presentation, node, hash});
  }
  void Opacity(uint32_t node, float value) {
    OpacityIn(state.engine, node, value);
  }
  void Color(uint32_t node, uint32_t rgb, bool text = false) {
    if (!node) return;
    Write(base, scratch + 768, 0x820307A4);
    Write(base, scratch + 772, node);
    Write(base, scratch + 776, state.engine);
    if (text) {
      Call(0x82F37BE0, {scratch + 768, rgb});
    } else {
      // Static FH2 materials may have no animated diffuse-channel records.
      // FH1's renderer uses these normalized defaults before optional channel
      // overrides (82F20CA0); the native setter alone silently skips them.
      const auto instance = Read(base, node + 4);
      const auto material = instance ? Read(base, instance) : 0;
      if (material) {
        for (unsigned channel = 0; channel < 3; ++channel) {
          const auto offset = 0x70 + channel * 4;
          const float current = std::bit_cast<float>(Read(base, material + offset));
          if (std::isfinite(current) && current >= 0) {
            const float value = float((rgb >> (16 - channel * 8)) & 255) / 255.f;
            Write(base, material + offset, std::bit_cast<uint32_t>(value));
            Write(base, material + 0xA0 + channel * 4,
                  std::bit_cast<uint32_t>(value));
          }
        }
      }
      Call(0x82F37C40, {scratch + 768, rgb});
    }
  }
  void ShowIn(uint32_t presentation, uint32_t node, bool visible) {
    if (!node) return;
    Write(base, scratch + 768, 0x820307A4);
    Write(base, scratch + 772, node);
    Write(base, scratch + 776, presentation);
    Call(0x82F35B48, {scratch + 768, uint32_t(visible)});
    const auto instance = Read(base, node + 4);
    if (instance && Read(base, instance + 4) == node &&
        bool(GuestPtr(base, instance + 8)[0]) != visible) {
      const auto dirty = Call(Read(base, Read(base, presentation) + 0x7C),
                              {presentation});
      Write(base, scratch + 800, node);
      if (dirty) Call(0x82FE57F0, {dirty, scratch + 800});
    }
  }
  void Show(uint32_t node, bool visible) {
    ShowIn(state.engine, node, visible);
  }
  void PositionIn(uint32_t presentation, uint32_t node, float x, float y) {
    if (!node) return;
    const auto instance = Read(base, node + 4);
    if (!instance || Read(base, instance + 4) != node) return;
    for (unsigned axis = 0; axis < 2; ++axis) {
      const float target = axis ? y : x;
      const auto hash = Call(0x82F354E0, {String(axis ? "position.y" : "position.x")});
      if (Call(0x82FEF130, {node, hash})) {
        ctx.f1.f64 = target;
        Call(0x82F34DE0, {presentation, node, hash});
        continue;
      }
      // Static imported nodes can omit scalar animation properties. Match
      // the native instance setter and queue a transform rebuild so parent
      // scale/rotation and every descendant are handled by the engine.
      Write(base, instance + 0xC + axis * 4, std::bit_cast<uint32_t>(target));
      GuestPtr(base, instance + 0xFB)[0] |= 0x20;
      Write(base, scratch + 800, node);
      const auto dirty = Call(Read(base, Read(base, presentation) + 0x7C),
                              {presentation});
      if (dirty) Call(0x82FE57F0, {dirty, scratch + 800});
    }
  }
  void Position(uint32_t node, float x, float y) {
    PositionIn(state.engine, node, x, y);
  }
  void ScaleIn(uint32_t presentation, uint32_t node, float x, float y) {
    if (!node) return;
    const auto instance = Read(base, node + 4);
    if (!instance || Read(base, instance + 4) != node) return;
    for (unsigned axis = 0; axis < 2; ++axis) {
      const float target = axis ? y : x;
      const auto hash = Call(0x82F354E0, {String(axis ? "scale.y" : "scale.x")});
      const auto property = Call(0x82FEF130, {node, hash});
      if (property) {
        ctx.f1.f64 = target;
        Call(0x82F34DE0, {presentation, node, hash});
        continue;
      }
      // Static native meshes may keep their scale only in the instance's
      // transform cache. Use the same dirty rebuild as PositionIn so their
      // complete subtree receives the new world transform.
      Write(base, instance + 0x24 + axis * 4, std::bit_cast<uint32_t>(target));
      GuestPtr(base, instance + 0xFB)[0] |= 0x20;
      Write(base, scratch + 800, node);
      const auto dirty = Call(Read(base, Read(base, presentation) + 0x7C),
                              {presentation});
      if (dirty) Call(0x82FE57F0, {dirty, scratch + 800});
    }
  }
  void FontFrom(uint32_t node, uint32_t source) {
    if (!node || !source) return;
    const auto hash = Call(0x82F354E0, {String("font")});
    const auto target_property = Call(0x82FEF130, {node, hash});
    const auto source_property = Call(0x82FEF130, {source, hash});
    if (!target_property || !source_property ||
        (Read(base, target_property) & 0x1C) != 0x14 ||
        (Read(base, source_property) & 0x1C) != 0x14) return;
    // Reuse the scene-owned FH1 font handle through the native string-property
    // setter, which also invalidates the target's cached text geometry.
    Call(0x82FF2748, {state.access, node, target_property,
                      Read(base, source_property + 4)});
  }
  void ImagePositionUIn(uint32_t presentation, uint32_t node, float value) {
    if (!node) return;
    const auto hash = Call(0x82F354E0, {String("positionu")});
    if (Call(0x82FEF130, {node, hash})) {
      ctx.f1.f64 = value;
      Call(0x82F34DE0, {presentation, node, hash});
      return;
    }
    // Native image wrappers are type 7 (82F2D918). Constant image properties
    // live in their render object, unlike mesh transform caches. Rebuild the
    // complete UV matrix through the native image updater after changing U.
    const auto wrapper = Read(base, node + 4);
    if (!wrapper || Read(base, wrapper + 4) != node ||
        GuestPtr(base, wrapper + 0xA)[0] != 7) {
      REXLOG_WARN("FH1 username paper: image wrapper rejected node={:08X}", node);
      return;
    }
    const auto image = Read(base, wrapper);
    if (!image) return;
    const auto previous = std::bit_cast<float>(Read(base, image + 0xB4));
    Write(base, image + 0xB4, std::bit_cast<uint32_t>(value));
    Call(0x82F1D8F0, {0, wrapper, node});
    REXLOG_INFO("FH1 username paper: UV positionu {} -> {}", previous,
                std::bit_cast<float>(Read(base, image + 0xB4)));
  }
  void TextSize(uint32_t node, float size) {
    if (!node) return;
    const auto hash = Call(0x82F354E0, {String("size")});
    ctx.f1.f64 = size;
    Call(0x82F34DE0, {state.engine, node, hash});
  }
  void TextAlignment(uint32_t node, uint32_t alignment) {
    if (!node) return;
    const auto hash = Call(0x82F354E0, {String("horzalign")});
    const auto property = Call(0x82FEF130, {node, hash});
    if (property && (Read(base, property) & 0x1C) == 4)
      Call(0x82FF2748, {state.access, node, property, alignment});
  }
  void Event(uint32_t contract, uint32_t hash) {
    if (!contract) return;
    auto& wrapper = event_wrappers[contract];
    if (!wrapper) wrapper = REX_KERNEL_MEMORY()->SystemHeapAlloc(28);
    if (!wrapper) return;
    for (unsigned offset = 0; offset < 28; offset += 4)
      Write(base, wrapper + offset, 0);
    Write(base, wrapper, 0x8225FED4);
    Write(base, wrapper + 4, 0x8225FECC);
    Write(base, wrapper + 8, state.engine);
    Write(base, wrapper + 12, contract);
    Write(base, wrapper + 16, Read(base, contract + 12));
    Call(0x82F35438, {wrapper, hash});
  }
};

std::optional<uint16_t> HeldButtons(Native& ui) {
  if (!ui.scratch) return std::nullopt;
  ui.ctx.r3.u64 = 0;
  ui.ctx.r4.u64 = 0;
  ui.ctx.r5.u64 = ui.scratch;
  __imp__XamInputGetState(ui.ctx, ui.base);
  if (ui.ctx.r3.u32) return std::nullopt;
  auto buttons = uint16_t(Read(ui.base, ui.scratch + 4) >> 16);
  const auto axes = Read(ui.base, ui.scratch + 8);
  const auto x = int16_t(axes >> 16);
  const auto y = int16_t(axes & 0xFFFF);
  if (x < -16000) buttons |= 4;
  if (x > 16000) buttons |= 8;
  if (y > 16000) buttons |= 1;
  if (y < -16000) buttons |= 2;
  return buttons;
}
void BlockHeldInput(Native& ui) {
  // A child may close while B is still down; opening pause can likewise leave
  // Start down. Require those existing presses to be released before reuse.
  const auto held = HeldButtons(ui).value_or(0xFFFF);
  state.previous = held;
  state.blocked = held;
}

struct Tile {
  uint32_t action;
  const char* label;
  const char* description;
};
constexpr Tile home[] = {
    {2, "WORLD MAP", "Explore Colorado"},
    {0x7110, "SETTINGS", "Audio, graphics, controls and difficulty"},
    {0x7108, "MY CARS", "Choose a car from your garage"},
    {0x7111, "FESTIVAL\nPROGRESS", ""},
    {11, "PHOTO MODE", "Capture your Horizon"},
    {12, "MY\nPROFILE", "Your Horizon career"}};
constexpr Tile map[] = {{2, "WORLD MAP", "Open the Colorado map"}};
constexpr Tile cars[] = {
    {0x7108, "CHANGE CAR", "Choose from your owned cars"},
    {0x7112, "GO TO GARAGE", "Fast travel for car upgrades"},
    {0x7113, "GO TO AUTOSHOW", "Fast travel to buy cars"}};
constexpr Tile settings[] = {
    {0x7104, "DIFFICULTY", "Driving assists and difficulty"},
    {0x7000, "GRAPHICS", "Display and graphics settings"},
    {0x710C, "AUDIO", "Music, voice and sound"},
    {0x710D, "CONTROLS", "Controller layout"},
    {0x710E, "HUD", "Driving display and notifications"},
    {0x710F, "VIDEO", "Brightness and subtitles"}};
std::span<const Tile> Tiles() {
  switch (state.page) {
    case 1: return map;
    case 2: return cars;
    case 3: return settings;
    default: return home;
  }
}
bool NativeActionAvailable(uint8_t* base, uint32_t action) {
  if (!state.screen) return false;
  const auto begin = Read(base, state.screen + 0x19C);
  const auto end = Read(base, state.screen + 0x1A0);
  if (!begin || end < begin || end - begin > 1024 || (end - begin) % 4)
    return false;
  for (auto entry = begin; entry < end; entry += 4) {
    const auto row = Read(base, entry);
    if (row && Read(base, row + 8) == action)
      return GuestPtr(base, row + 0x14)[0] != 0;
  }
  return false;
}
bool ActionAvailable(uint8_t* base, uint32_t action) {
  switch (action) {
    case fh1_pause_actions::kResume:
    case 0x7000: // Graphics
    case 0x7104: // Difficulty
    case 0x710C: // Audio
    case 0x710D: // Controls
    case 0x710E: // HUD
    case 0x710F: // Video
    case 0x7110: // Settings page
    case 0x7111: // Live festival progress panel
      return true;
    case 0x7112:
      return NativeActionAvailable(base, 2) && state.garage_available;
    case 0x7113:
      return NativeActionAvailable(base, 2) && state.autoshow_available;
    case 0x7108:
      // Changing cars follows the native free-roam map availability gate.
      return NativeActionAvailable(base, 2);
    default:
      return NativeActionAvailable(base, action);
  }
}
void UpdateFooter(Native& ui, bool force = false) {
  if (!state.visible || !state.screen) return;
  if (!force && ++state.footer_refresh_frames < 30) return;
  state.footer_refresh_frames = 0;
  const bool available = Read(ui.base, state.screen + 0x1DC) &&
      NativeActionAvailable(ui.base, 2) &&
      fh1_car_recovery::Available(ui.ctx, ui.base);
  if (!force && state.footer_ready && available == state.recovery_available) return;
  const auto footer = ui.Call(0x82746638, {state.screen});
  if (!footer) return;
  // Rebuild instead of appending repeatedly, retaining FH1's native A/B model.
  ui.Call(0x827D6348, {state.screen});
  if (available) {
    if (const auto text = ui.TextRef("RESET CAR POSITION")) {
      // Native HELP_BUTTON glyph 14 is the left-thumbstick click artwork.
      ui.Call(0x8289E2B0, {footer, 14, text});
      ui.Call(0x824CEAB0, {text});
    }
  }
  ui.Call(Read(ui.base, Read(ui.base, footer + 0x6C) + 0x28), {footer + 0x6C});
  state.recovery_available = available;
  state.footer_ready = true;
  REXLOG_INFO("FH1 pause footer: reset available={}", available);
}
uint32_t EnabledPages(uint8_t* base) {
  // Home and Settings remain available in races; Map and Cars follow FH1.
  return NativeActionAvailable(base, 2) ? 0xF : 0x9;
}
uint32_t FirstAvailableSelection(uint8_t* base) {
  const auto tiles = Tiles();
  for (uint32_t i = 0; i < tiles.size(); ++i)
    if (ActionAvailable(base, tiles[i].action)) return i;
  return 0;
}
void NormalizeSelection(uint8_t* base) {
  const auto tiles = Tiles();
  if (state.selection >= tiles.size() ||
      !ActionAvailable(base, tiles[state.selection].action))
    state.selection = FirstAvailableSelection(base);
}
std::vector<uint32_t> Descendants(uint8_t* base, uint32_t root, uint32_t key) {
  std::vector<uint32_t> result;
  std::vector<uint32_t> pending{root};
  unsigned count = 0;
  while (!pending.empty() && count++ < 10000) {
    const auto node = pending.back();
    pending.pop_back();
    if (!node) continue;
    if (Read(base, node) == key) result.push_back(node);
    unsigned children = 0;
    for (auto child = Read(base, node + 20); child && children++ < 1000;
         child = Read(base, child + 16))
      pending.push_back(child);
  }
  return result;
}
std::string GameCompletionLabel(float percentage) {
  const auto tenths = std::clamp(int(std::lround(percentage * 10.f)), 0, 1000);
  if (tenths == 1000) return "100%";
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "%";
}
void BindFestivalGauge(Native& ui);
void UpdateFestivalProgress(Native& ui, bool force = false) {
  if (!ui.scratch || !state.visible || state.page != 0 ||
      festival_progress_nodes.empty()) return;
  // Completion queries native career data; pause entry/child return force an
  // immediate refresh, with only occasional refreshes while the menu is idle.
  if (!force && ++festival_progress_frames < 300) return;
  festival_progress_frames = 0;
  if (!festival_gauge.controller) BindFestivalGauge(ui);
  const auto game = Read(ui.base, 0x833CAAE4);
  if (!game || !ui.Call(0x82F12410, {game}) ||
      !Read(ui.base, game + 0x10) || !Read(ui.base, game + 0x6C)) return;
  // Mirror Statistics General (8284CC88): USER0's profile and the native
  // completion getter, including its additional completion counts. The getter
  // returns percentage units in f1, independently of wristband level or XP.
  const auto facade = ui.Call(0x825EC700, {});
  const auto facade_table = facade ? Read(ui.base, facade) : 0;
  if (!facade_table) return;
  const auto player = ui.Call(Read(ui.base, facade_table + 0x28), {facade});
  const auto player_table = player ? Read(ui.base, player) : 0;
  if (!player_table) return;
  const auto profile = ui.Call(Read(ui.base, player_table + 0x58), {player});
  if (!profile) return;
  ui.Call(0x8269E150, {profile, 1});
  const auto raw_percentage = ui.ctx.f1.f64;
  if (!std::isfinite(raw_percentage)) return;
  const float percentage = float(std::clamp(raw_percentage, 0.0, 100.0));
  festival_gauge.fraction = percentage / 100.f;
  const auto label = GameCompletionLabel(percentage);
  if (!force && label == festival_progress_text) return;
  festival_progress_text = label;
  for (auto node : festival_progress_nodes)
    ui.Text(node, "GAME COMPLETION\nVIEW STATISTICS");
  if (festival_gauge.number) {
    ui.Text(festival_gauge.number, label);
    ui.TextSize(festival_gauge.number, label.size() > 4 ? 16.f : 20.f);
  }
  if (festival_gauge.percent) ui.Text(festival_gauge.percent, "");
  if (festival_gauge.controller) {
    ui.ctx.f1.f64 = festival_gauge.fraction;
    ui.Call(0x82F36650, {festival_gauge.controller + 0xA4});
  }
  REXLOG_INFO("FH2 pause game completion: native={} label={} fraction={}",
              percentage, label, festival_gauge.fraction);
}
void BindFestivalGauge(Native& ui) {
  const std::string path = "parent.Layer.Grid_Menu.Group.Grid_Menu_01.Group.RoadtripProgress";
  const auto root = ui.Node(path);
  festival_gauge.root = root;
  if (!root) {
    if (state.page == 0) REXLOG_WARN("FH1 festival gauge: native ring is not loaded");
    return;
  }
  const bool visible = state.page == 0;
  ui.Show(root, visible);
  ui.Opacity(root, visible ? 100.f : 0.f);
  if (!visible) return;
  ui.Position(root, 378.f, 93.f);
  ui.ScaleIn(state.engine, root, 1.075f, 1.075f);
  // The shared grid owns this component so it can load before later tiles.
  // Keep its ring in front of the tile paper in the native depth buffer.
  const auto root_instance = Read(ui.base, root + 4);
  const auto z_hash = ui.Call(0x82F354E0, {ui.String("position.z")});
  if (ui.Call(0x82FEF130, {root, z_hash})) {
    ui.ctx.f1.f64 = -3.f;
    ui.Call(0x82F34DE0, {state.engine, root, z_hash});
  } else if (root_instance && Read(ui.base, root_instance + 4) == root) {
    Write(ui.base, root_instance + 0x14, std::bit_cast<uint32_t>(-3.f));
    GuestPtr(ui.base, root_instance + 0xFB)[0] |= 0x20;
    const auto dirty = ui.Call(Read(ui.base, Read(ui.base, state.engine) + 0x7C), {state.engine});
    Write(ui.base, ui.scratch + 800, root);
    if (dirty) ui.Call(0x82FE57F0, {dirty, ui.scratch + 800});
  }
  const auto inner = path + ".RoadtripProgress";
  festival_gauge.number = ui.Node(inner + ".Text_Percent_Number");
  festival_gauge.percent = ui.Node(inner + ".Text_Percent");
  // Keep the complete percentage in the original centered number field. The
  // donor's separate percent label is authored at zero opacity.
  ui.Color(festival_gauge.number, 0xFFFFFF, true);
  ui.TextSize(festival_gauge.number,
      GameCompletionLabel(festival_gauge.fraction * 100.f).size() > 4 ? 16.f : 20.f);
  for (const auto node : Descendants(ui.base, root, Hash("Material19808")))
    ui.Color(node, 0xFF007A);
  for (const auto node : Descendants(ui.base, root, Hash("Material19800")))
    ui.Color(node, 0xFFFFFF);
  std::vector<ProgressController> controls;
  {
    std::lock_guard lock(contracts_mutex);
    controls = progress_controllers[state.engine];
  }
  for (const auto& entry : controls) {
    if (entry.node != root || Read(ui.base, entry.controller) != 0x8207ED44 ||
        Read(ui.base, entry.controller + 0xA8) != root ||
        Read(ui.base, entry.controller + 0xAC) != state.engine) continue;
    if (festival_gauge.controller != entry.controller) {
      festival_gauge.controller = entry.controller;
      // Select the ring's original PROGRESS slide and its native manual mode.
      ui.Call(0x828BAED8, {entry.controller});
      ui.ctx.f1.f64 = festival_gauge.fraction;
      ui.Call(0x82F36650, {entry.controller + 0xA4});
    }
    break;
  }
  REXLOG_INFO("FH1 festival gauge: root={:08X} controller={:08X} number={:08X} percent={:08X}",
              root, festival_gauge.controller, festival_gauge.number, festival_gauge.percent);
}
void GarageHeaderVisibility(Native& ui, bool visible) {
  if (!garage_header.ready || garage_header.visible == visible) return;
  const auto presentation = garage_header.presentation;
  // These are the native presentation suppression flags. Resource loading
  // starts with both set, so the donor cannot draw before its siblings hide.
  ui.Call(0x82FEB480, {presentation, uint32_t(!visible)});
  ui.Call(0x82FEB488, {presentation, uint32_t(!visible)});
  if (garage_header.render_root)
    GuestPtr(ui.base, garage_header.render_root + 0xFC)[0] = uint8_t(visible);
  if (garage_header.render_owner)
    GuestPtr(ui.base, garage_header.render_owner + 0x144)[0] = uint8_t(visible);
  // Native scene activation marks the renderer's presentation list dirty.
  // Updating the tree's visibility alone leaves newly loaded donors unwalked.
  const auto manager = ui.Call(
      Read(ui.base, Read(ui.base, presentation) + 0x54), {presentation});
  const auto renderer = manager ? ui.Call(
      Read(ui.base, Read(ui.base, manager) + 0x3C), {manager}) : 0;
  if (renderer) GuestPtr(ui.base, renderer + 0x110)[0] = 1;
  garage_header.visible = visible;
  REXLOG_INFO("FH1 garage header: render={} presentation={:08X}",
              visible, presentation);
}
void StartGarageHeader(Native& ui) {
  if (garage_header.preloader) return;
  // The last word retains the resource's native in-use reference separately
  // from the asynchronous request owned by the 0x50-byte preloader.
  const auto preloader = ui.Call(0x824265C8, {0x54});
  if (!preloader) return;
  ui.Call(0x82F273B0, {preloader});
  Write(ui.base, preloader + 0x50, 0);
  constexpr char scene[] =
      "game:\\media\\ui\\scenes\\ui4\\FH1_PAUSE_PLAYER_BAR.bgf";
  ui.Call(0x8243D4D8, {preloader + 4, ui.String(scene), sizeof(scene) - 1});
  ui.Call(0x82CC2450, {preloader + 4});
  garage_header.preloader = preloader;
  ui.Call(0x82F30490, {preloader, 0x60000});
  REXLOG_INFO("FH1 garage header: requested native scene {} handle={:08X}",
              scene, preloader);
}
// Drain only native registration work. The complete manager update also
// reapplies every presentation's authored properties, including the pause UI.
void DrainHeaderPresentations(Native& ui, uint32_t manager) {
  if (!manager || !Read(ui.base, manager + 0x27C)) return;
  ui.Call(0x82FE3F28, {manager + 0x284});
  while (const auto count = Read(ui.base, manager + 0x27C)) {
    const auto pair = Read(ui.base, manager + 0x274) + (count - 1) * 8;
    const auto presentation = Read(ui.base, pair);
    const auto renderer = Read(ui.base, pair + 4);
    Write(ui.base, manager + 0x27C, count - 1);
    if (ui.Call(Read(ui.base, Read(ui.base, presentation) + 8), {presentation})) {
      ui.Call(0x82FF2DD8, {manager + 0x28, presentation});
      const auto adapter = Read(ui.base, manager + 0x20C);
      ui.Call(Read(ui.base, Read(ui.base, adapter) + 8), {adapter, renderer});
    }
  }
  ui.Call(0x82FE3F50, {manager + 0x284});
  if (const auto callback = Read(ui.base, manager + 0x248))
    ui.Call(callback, {3, 0, 0, Read(ui.base, manager + 0x24C)});
}

void RetainHeaderControllers(Native& ui, uint32_t presentation, uint32_t header) {
  const auto manager = Read(ui.base, presentation + 0x20);
  if (!manager) return;
  // These dispatch vectors borrow their pairs from the ownership map at +8.
  // Keep that map intact so native destruction still releases every component.
  for (const auto offset : {0x28u, 0x38u}) {
    const auto begin = Read(ui.base, manager + offset);
    const auto end = Read(ui.base, manager + offset + 4);
    if (!begin || end < begin || end - begin > 8192 || (end - begin) % 8) continue;
    auto output = begin;
    for (auto entry = begin; entry < end; entry += 8) {
      const auto wrapper = Read(ui.base, entry);
      auto node = wrapper ? Read(ui.base, wrapper + 0x10) : 0;
      unsigned depth = 0;
      while (node && node != header && depth++ < 100)
        node = Read(ui.base, node + 12);
      if (node != header) continue;
      Write(ui.base, output, wrapper);
      Write(ui.base, output + 4, Read(ui.base, entry + 4));
      output += 8;
    }
    Write(ui.base, manager + offset + 4, output);
  }
}

void BindPlayerIdentity(Native& ui) {
  if (player_identity.bar) return;
  const auto bars = Descendants(ui.base, ui.Node("parent"), Hash("PlayerBar"));
  if (bars.empty()) return;
  player_identity.bar = bars.front();
  const auto names = Descendants(ui.base, player_identity.bar, Hash("TEXT_Player"));
  const auto statuses = Descendants(ui.base, player_identity.bar, Hash("TEXT_T"));
  if (names.empty() || statuses.empty()) return;
  player_identity.name = names.front();
  player_identity.status = statuses.front();

  // Keep native ownership intact, but stop the imported career/token controllers
  // from overwriting the two fields now used for the active profile identity.
  const auto manager = Read(ui.base, state.engine + 0x20);
  for (const auto offset : {0x28u, 0x38u}) {
    const auto begin = Read(ui.base, manager + offset);
    const auto end = Read(ui.base, manager + offset + 4);
    if (!begin || end < begin || end - begin > 8192 || (end - begin) % 8) continue;
    auto output = begin;
    for (auto entry = begin; entry < end; entry += 8) {
      const auto wrapper = Read(ui.base, entry);
      auto node = wrapper ? Read(ui.base, wrapper + 0x10) : 0;
      unsigned depth = 0;
      while (node && node != player_identity.bar && depth++ < 100)
        node = Read(ui.base, node + 12);
      if (node == player_identity.bar) continue;
      Write(ui.base, output, wrapper);
      Write(ui.base, output + 4, Read(ui.base, entry + 4));
      output += 8;
    }
    Write(ui.base, manager + offset + 4, output);
  }

  for (const auto text : {player_identity.name, player_identity.status}) {
    for (auto node = text; node; node = Read(ui.base, node + 12)) {
      if (std::find(player_identity.visible.begin(), player_identity.visible.end(), node) ==
          player_identity.visible.end()) player_identity.visible.push_back(node);
      if (node == player_identity.bar) break;
    }
  }
  std::vector<uint32_t> pending{player_identity.bar};
  while (!pending.empty()) {
    const auto node = pending.back();
    pending.pop_back();
    for (auto child = Read(ui.base, node + 20); child; child = Read(ui.base, child + 16)) {
      if (std::find(player_identity.visible.begin(), player_identity.visible.end(), child) !=
          player_identity.visible.end()) pending.push_back(child);
      else player_identity.hidden.push_back(child);
    }
  }
  // Isolate the two original text nodes from the source header's stacked layout.
  // All remaining branches, including its duplicate car/credit/wristband, stay hidden.
  for (const auto node : player_identity.visible) ui.Position(node, 0.f, 0.f);
  ui.Position(player_identity.name, 450.f, 310.f);
  ui.Position(player_identity.status, 450.f, -310.f);
  for (const auto node : {player_identity.name, player_identity.status}) {
    if (!theme_descriptions.empty()) ui.FontFrom(node, theme_descriptions.front());
    ui.TextSize(node, node == player_identity.name ? 18.f : 14.f);
    ui.TextAlignment(node, 1);
    ui.Color(node, node == player_identity.name ? 0xFFFFFF : 0xD9D9D9, true);
    ui.Text(node, "");
  }
  REXLOG_INFO("FH2 pause identity: bound name={:08X} status={:08X}",
              player_identity.name, player_identity.status);
}

void UpdatePlayerIdentity(Native& ui, bool force = false) {
  if (!player_identity.name) return;
  for (const auto node : player_identity.hidden) ui.Show(node, false);
  for (const auto node : player_identity.visible) {
    ui.Show(node, state.visible);
    ui.Opacity(node, 100);
  }
  ui.Show(player_identity.status, state.visible && !player_identity.connection.empty());
  if (garage_header.username) ui.Show(player_identity.name, false);
  if (!state.visible || (!force && ++player_identity.frames < 30)) return;
  player_identity.frames = 0;
  // The game's host flag also exists in an offline world, so only advertise
  // confirmed transport connectivity until a session membership gate is known.
  const std::string connection = LiveConnected() ? "ONLINE" : "";
  if (force || connection != player_identity.connection) {
    player_identity.connection = connection;
    ui.Text(player_identity.status, connection);
    ui.Show(player_identity.status, !connection.empty());
  }
  ui.ctx.r3.u64 = 0;
  ui.ctx.r4.u64 = ui.scratch + 896;
  ui.ctx.r5.u64 = 16;
  __imp__XamUserGetName(ui.ctx, ui.base);
  if (ui.ctx.r3.u32) return;
  std::string gamertag;
  for (unsigned i = 0; i < 15 && GuestPtr(ui.base, ui.scratch + 896)[i]; ++i)
    gamertag += char(GuestPtr(ui.base, ui.scratch + 896)[i]);
  if (force || gamertag != player_identity.gamertag) {
    player_identity.gamertag = gamertag;
    ui.Text(player_identity.name, gamertag);
  }
}

void BindTiles(Native& ui);
void BindGarageUsername(Native& ui) {
  // Reuse the donor's unused native timer label and white paper image. Its
  // controllers are excluded by RetainHeaderControllers, so this is solely a
  // presentation widget and cannot introduce a timer or input owner.
  const auto names = Descendants(ui.base, garage_header.root, Hash("TEXT_TIME"));
  if (names.empty()) return;
  const auto group = Read(ui.base, names.front() + 12);
  // BGF gives FBF mesh 0x50C ("Pill_white") a different serialized node name.
  // Use its actual runtime identity rather than the renderer's resource label.
  const auto papers = Descendants(ui.base, group, 0xA17E639E);
  if (papers.empty()) {
    REXLOG_WARN("FH1 username paper: native timer paper missing");
    return;
  }
  garage_header.username = names.front();
  garage_header.username_paper = papers.front();
  for (auto node = group; node; node = Read(ui.base, node + 12)) {
    garage_header.username_ancestry.push_back(node);
    if (node == garage_header.root) break;
  }
  for (const auto parent : garage_header.username_ancestry) {
    // Layer and root also own the retained player bar and camera.
    if (parent == garage_header.root || Read(ui.base, parent) == Hash("Layer"))
      continue;
    for (auto child = Read(ui.base, parent + 20); child;
         child = Read(ui.base, child + 16)) {
      if (child == garage_header.username || child == garage_header.username_paper ||
          std::find(garage_header.username_ancestry.begin(),
                    garage_header.username_ancestry.end(), child) !=
              garage_header.username_ancestry.end()) continue;
      garage_header.username_hidden.push_back(child);
    }
  }
  for (const auto node : garage_header.username_ancestry)
    std::erase(garage_header.hidden, node);
  REXLOG_INFO("FH1 username paper: text={:08X} paper={:08X}",
              garage_header.username, garage_header.username_paper);
}
void UpdateGarageUsername(Native& ui) {
  if (!garage_header.username) return;
  const auto presentation = garage_header.presentation;
  for (const auto node : garage_header.username_hidden) ui.ShowIn(presentation, node, false);
  for (const auto node : garage_header.username_ancestry) {
    ui.ShowIn(presentation, node, true);
    if (node != garage_header.root && Read(ui.base, node) != Hash("Layer"))
      ui.OpacityIn(presentation, node, 100.f);
  }
  ui.ShowIn(presentation, garage_header.username, true);
  ui.ShowIn(presentation, garage_header.username_paper, true);
  ui.OpacityIn(presentation, garage_header.username, 100.f);
  ui.OpacityIn(presentation, garage_header.username_paper, 100.f);
  if (!garage_header.username_styled) {
    for (const auto node : garage_header.username_ancestry) {
      if (node != garage_header.root && Read(ui.base, node) != Hash("Layer"))
        ui.PositionIn(presentation, node, 0.f, 0.f);
    }
    const auto group = Read(ui.base, garage_header.username + 12);
    ui.PositionIn(presentation, group, 450.f, 310.f);
    ui.PositionIn(presentation, garage_header.username, 0.f, 0.f);
    ui.PositionIn(presentation, garage_header.username_paper, 0.f, 0.f);
    const auto scalar = [&](uint32_t node, const char* property, float value) {
      const auto hash = ui.Call(0x82F354E0, {ui.String(property)});
      ui.ctx.f1.f64 = value;
      ui.Call(0x82F34DE0, {presentation, node, hash});
    };
    // Leave room around a full-length gamertag, including the paper's
    // transparent torn edge. The name retains its original 18px FH1 font.
    ui.ScaleIn(presentation, garage_header.username_paper, 2.6f, .36f);
    // The original timer clips this image to a short numeric field by setting
    // U to .926. The username uses the complete native paper texture instead.
    for (const auto image : Descendants(ui.base, garage_header.username_paper,
                                        0xDE7BB086))
      ui.ImagePositionUIn(presentation, image, 0.f);
    const auto paper_instance = Read(ui.base, garage_header.username_paper + 4);
    REXLOG_INFO("FH1 username paper: scale=({}, {})",
                std::bit_cast<float>(Read(ui.base, paper_instance + 0x24)),
                std::bit_cast<float>(Read(ui.base, paper_instance + 0x28)));
    scalar(garage_header.username, "size", 18.f);
    // Native text boxes use normalized layout units, not mesh pixels. Keep
    // an 18px maximum and let native text fitting shrink wide gamertags into
    // the 220px interior, leaving padding inside the 260px paper artwork.
    constexpr float name_width = 220.f / 1200.f;
    constexpr float name_height = .05f;
    scalar(garage_header.username, "boxwidth", name_width);
    scalar(garage_header.username, "boxheight", name_height);
    const auto align_hash = ui.Call(0x82F354E0, {ui.String("horzalign")});
    const auto align_property = ui.Call(0x82FEF130,
        {garage_header.username, align_hash});
    const auto access = ui.Call(Read(ui.base, Read(ui.base, presentation) + 0x1C),
                               {presentation});
    if (align_property && (Read(ui.base, align_property) & 0x1C) == 4)
      ui.Call(0x82FF2748, {access, garage_header.username, align_property, 1});
    // Timer text is authored as unbounded texttype 0. Enable the native
    // bounded shrink-to-fit path, keeping the gamertag on a single line.
    for (const auto [name, value] : {std::pair{"texttype", 1u},
                                    std::pair{"wordwrap", 0u}}) {
      const auto hash = ui.Call(0x82F354E0, {ui.String(name)});
      const auto property = ui.Call(0x82FEF130, {garage_header.username, hash});
      if (property)
        ui.Call(0x82FF2748, {access, garage_header.username, property, value});
    }
    const auto instance = Read(ui.base, garage_header.username + 4);
    const auto text = instance ? Read(ui.base, instance) : 0;
    if (text && Read(ui.base, instance + 4) == garage_header.username) {
      Write(ui.base, text + 0x70, std::bit_cast<uint32_t>(name_height));
      Write(ui.base, text + 0x74, std::bit_cast<uint32_t>(name_width));
      Write(ui.base, text + 0x80, 18);
      Write(ui.base, text + 0x7C, 1);
      Write(ui.base, text + 0xDC, 2);
      GuestPtr(ui.base, text + 0x104)[0] = 0;
      Write(ui.base, text + 0xC0, 0);
      Write(ui.base, text + 0xC4, 0);
      GuestPtr(ui.base, text + 0x105)[0] = 1;
    }
    Write(ui.base, ui.scratch + 768, 0x820307A4);
    Write(ui.base, ui.scratch + 772, garage_header.username);
    Write(ui.base, ui.scratch + 776, presentation);
    ui.Call(0x82F37BE0, {ui.scratch + 768, 0x292929});
    garage_header.username_styled = true;
  }
  if (garage_header.username_value != player_identity.gamertag) {
    garage_header.username_value = player_identity.gamertag;
    garage_header.username_fit_frames = 0;
    ui.TextIn(presentation, garage_header.username, garage_header.username_value);
  }
  if (!garage_header.username_value.empty() && ++garage_header.username_fit_frames == 3) {
    const auto instance = Read(ui.base, garage_header.username + 4);
    const auto text = instance ? Read(ui.base, instance) : 0;
    if (text)
      REXLOG_INFO("FH1 username fit: glyphs=({}, {}) font={}",
                  std::bit_cast<float>(Read(ui.base, text + 0xF0)),
                  std::bit_cast<float>(Read(ui.base, text + 0xF4)),
                  Read(ui.base, text + 0xE0));
  }
}
void UpdateGarageHeader(Native& ui) {
  if (!garage_header.preloader) return;
  if (!garage_header.ready) {
    ++garage_header.frames;
    const auto reference = garage_header.preloader + 0x50;
    if (!Read(ui.base, reference))
      ui.Call(0x82D0CAD0, {reference, garage_header.preloader + 0x20});
    const auto resource = Read(ui.base, reference);
    const auto presentation = resource ? Read(ui.base, resource + 0x74) : 0;
    if (!presentation) {
      if (garage_header.frames == 120 || garage_header.frames == 600)
        REXLOG_INFO("FH1 garage header: pending resource={:08X} state={} "
                    "target={} children={:08X}/{:08X}/{:08X}", resource,
                    resource ? GuestPtr(ui.base, resource + 0x28)[0] : 0,
                    resource ? GuestPtr(ui.base, resource + 0x29)[0] : 0,
                    resource ? Read(ui.base, resource + 0x78) : 0,
                    resource ? Read(ui.base, resource + 0x7C) : 0,
                    resource ? Read(ui.base, resource + 0x80) : 0);
      return;
    }
    // A resource can be ready while its presentation is still queued for
    // insertion. A pause overlay has no screen-load pump to drain that queue.
    // Drain it before resolving components or dispatching their first events.
    const auto scene_manager = ui.Call(
        Read(ui.base, Read(ui.base, presentation) + 0x54), {presentation});
    DrainHeaderPresentations(ui, scene_manager);
    const auto access = ui.Call(Read(ui.base, Read(ui.base, presentation) + 0x1C),
                                {presentation});
    const auto root = access
        ? ui.Call(Read(ui.base, Read(ui.base, access) + 0x48), {access}) : 0;
    if (!root) return;
    auto headers = Descendants(ui.base, root, Hash("PlayerBar"));
    if (headers.empty()) headers = Descendants(ui.base, root, Hash("PLAYER_BAR"));
    if (headers.empty()) {
      if (garage_header.frames == 120 || garage_header.frames == 600)
        REXLOG_WARN("FH1 garage header: loaded without PlayerBar root={:08X}", root);
      return;
    }
    const auto player_bar = headers.front();
    // Retain the original ancestry, transforms and all header descendants.
    // Only unrelated sibling branches are disabled; no geometry is copied.
    std::vector<uint32_t> ancestry;
    for (auto node = player_bar; node && ancestry.size() < 100;
         node = Read(ui.base, node + 12)) {
      ancestry.push_back(node);
      if (node == root) break;
    }
    if (ancestry.empty() || ancestry.back() != root) return;
    for (auto parent : ancestry) {
      if (parent == player_bar) continue;
      unsigned children = 0;
      for (auto child = Read(ui.base, parent + 20); child && children++ < 1000;
           child = Read(ui.base, child + 16)) {
        const auto name = Read(ui.base, child);
        const bool retained = name == Hash("Camera") || name == Hash("Light") ||
            std::find(ancestry.begin(), ancestry.end(), child) != ancestry.end();
        ui.ShowIn(presentation, child, retained);
        if (!retained) garage_header.hidden.push_back(child);
      }
    }
    garage_header.presentation = presentation;
    garage_header.root = root;
    garage_header.player_bar = player_bar;
    // The native FH1 credits strip is the complete BottomRow subtree. Keep
    // its text binding, paper artwork, rotation and automatic width together.
    const auto credit_rows = Descendants(ui.base, player_bar, Hash("BottomRow"));
    if (!credit_rows.empty() &&
        !Descendants(ui.base, credit_rows.front(), Hash("TEXT_CR")).empty())
      garage_header.credits = credit_rows.front();
    const auto renderer = ui.Call(Read(ui.base, Read(ui.base, presentation) + 8),
                                   {presentation});
    const auto metadata = renderer
        ? ui.Call(Read(ui.base, Read(ui.base, renderer) + 0x1C), {renderer}) : 0;
    garage_header.render_root = metadata ? Read(ui.base, metadata + 4) : 0;
    if (!garage_header.render_root) return;
    // Render material helpers follow metadata->owner->presentation. Use the
    // native base scene as that context, without adding it to the UI screen
    // stack or constructing the garage controller and its gameplay actions.
    const auto owner = ui.Call(0x824265C8, {0x148});
    if (!owner) return;
    const auto root_name = Read(ui.base, state.screen + 0x44) >= 16
        ? Read(ui.base, state.screen + 0x30) : state.screen + 0x30;
    ui.Call(0x82F2F398, {owner, presentation,
                         ui.String("108_C_BUY_CARSELECT"), root_name});
    Write(ui.base, owner + 0x10C, Read(ui.base, state.screen + 0x10C));
    // Native ordering is group * 100 + order. Draw the header immediately
    // after the pause presentation so its background blur cannot blur the UI.
    Write(ui.base, owner + 0x110, Read(ui.base, state.screen + 0x110) + 1);
    // Native draw passes select scene owners by this UI render group.
    Write(ui.base, owner + 0x11C, Read(ui.base, state.screen + 0x11C));
    garage_header.render_owner = owner;
    Write(ui.base, metadata, owner);
    RetainHeaderControllers(ui, presentation, player_bar);
    // Complete the native loaded-scene pass without inserting a screen into
    // the menu stack: initialize its contracts and register its root listener.
    ui.Call(0x82F219E8, {owner});
    GuestPtr(ui.base, owner + 0x4C)[0] = 1;
    const auto ui_manager = Read(ui.base, 0x833CAAC0);
    ui.Call(0x82700700, {owner + 0x5C, garage_header.preloader + 0x20});
    Write(ui.base, ui.scratch + 800, owner);
    ui.Call(0x82985560, {ui_manager + 0x24, Read(ui.base, ui_manager + 0x24),
                        ui.scratch + 800});
    // Match native scene attachment: request a complete UI update even when
    // the caller is using the paused-menu update path.
    if (ui_manager) GuestPtr(ui.base, ui_manager + 0x5D)[0] = 1;
    const auto manager = ui.Call(
        Read(ui.base, Read(ui.base, presentation) + 0x54), {presentation});
    const auto adapter = manager ? ui.Call(
        Read(ui.base, Read(ui.base, manager) + 0x3C), {manager}) : 0;
    const auto contains = [&](uint32_t entries, uint32_t count, uint32_t value) {
      if (!entries || count > 1024) return false;
      for (uint32_t i = 0; i < count; ++i)
        if (Read(ui.base, entries + i * 4) == value) return true;
      return false;
    };
    const bool in_presentations = manager && contains(
        Read(ui.base, manager + 0x48), Read(ui.base, manager + 0x50), presentation);
    const bool in_renderers = adapter && contains(
        Read(ui.base, adapter + 0x24), Read(ui.base, adapter + 0x2C), renderer);
    const auto pending = manager ? Read(ui.base, manager + 0x27C) : 0;
    if (manager && !pending && !in_presentations)
      ui.Call(0x82FF2DD8, {manager + 0x28, presentation});
    if (adapter && !pending && !in_renderers)
      ui.Call(0x82FE5EF0, {adapter, renderer});
    const auto registry = ui_manager ? Read(ui.base, ui_manager + 0x0C) : 0;
    if (registry) {
      ui.Call(0x82F293E0, {registry, renderer});
      garage_header.render_runtime = renderer;
    }
    ui.Call(0x82F1EA48, {owner, 1});
    ui.Call(0x82F21950, {owner});
    ui.Call(0x82F1EC20, {owner, Hash("INTRO") & 0x7FFFFFFF});
    ui.Call(0x82F1EC20, {owner, Hash("SHOW_PLAYER_BAR") & 0x7FFFFFFF});
    // Resolve from this live component manager rather than retaining factory
    // pointers across native scene destruction and guest-address reuse.
    const auto components = Read(ui.base, presentation + 0x20);
    const auto begin = Read(ui.base, components + 0x28);
    const auto end = Read(ui.base, components + 0x2C);
    if (begin && end >= begin && end - begin <= 8192 && (end - begin) % 8 == 0) {
      for (auto entry = begin; entry < end; entry += 8) {
        const auto wrapper = Read(ui.base, entry);
        const auto controller = Read(ui.base, entry + 4);
        if (wrapper && controller && Read(ui.base, wrapper + 0x10) == player_bar &&
            Read(ui.base, controller) == 0x8207EB54) {
          garage_header.controller = controller;
          break;
        }
      }
    }
    if (const auto controller = garage_header.controller) {
      ui.Call(0x82F36AB8, {controller, Read(ui.base, 0x833F44E8)});
    }
    // A paused asynchronous load can leave static nodes absent from the
    // dirty queue. Synchronize every authored node once through the renderer;
    // preserve all original visibility, transforms, materials and text styles.
    const auto dirty = ui.Call(Read(ui.base, Read(ui.base, presentation) + 0x7C),
                               {presentation});
    std::vector<uint32_t> pending_nodes{root};
    unsigned synchronized = 0;
    while (dirty && !pending_nodes.empty() && synchronized < 10000) {
      const auto node = pending_nodes.back();
      pending_nodes.pop_back();
      Write(ui.base, ui.scratch + 800, node);
      ui.Call(0x82FE57F0, {dirty, ui.scratch + 800});
      ++synchronized;
      unsigned siblings = 0;
      for (auto child = Read(ui.base, node + 20); child && siblings++ < 1000;
           child = Read(ui.base, child + 16))
        pending_nodes.push_back(child);
    }
    BindGarageUsername(ui);
    garage_header.ready = true;
    BindTiles(ui);
    UpdatePlayerIdentity(ui, true);
    REXLOG_INFO("FH1 garage header: ready resource={:08X} presentation={:08X} "
                "root={:08X} playerbar={:08X} renderroot={:08X} ancestors={}",
                resource, presentation, root, player_bar,
                garage_header.render_root, ancestry.size());
  }
  for (const auto node : garage_header.hidden)
    ui.ShowIn(garage_header.presentation, node, false);
  for (auto node = garage_header.player_bar; node;
       node = Read(ui.base, node + 12)) {
    ui.ShowIn(garage_header.presentation, node, true);
    if (node == garage_header.root) break;
  }
  GarageHeaderVisibility(ui, state.visible);
  if (garage_header.visible) {
    ui.Call(0x82FEE208, {garage_header.presentation});
    UpdateGarageUsername(ui);
    // PlayerBar's authored origin is (-253, 319). This places TEXT_CR's
    // original (+16, 0) anchor at approximately (1050, 67) on the 720p canvas.
    // The credits paper meets the username paper's painted lower edge.
    const auto credits_instance = garage_header.credits
        ? Read(ui.base, garage_header.credits + 4) : 0;
    if (credits_instance &&
        (Read(ui.base, credits_instance + 0xC) != std::bit_cast<uint32_t>(647.f) ||
         Read(ui.base, credits_instance + 0x10) != std::bit_cast<uint32_t>(-34.f)))
      ui.PositionIn(garage_header.presentation, garage_header.credits, 647.f, -34.f);
    const auto manager = Read(ui.base, garage_header.presentation + 0x14);
    const auto clock = ui.Call(Read(ui.base, Read(ui.base, manager + 8) + 0x10),
                               {manager + 8});
    // Consume controller and event changes before the native frame-begin
    // operation clears their dirty lists. Drawing remains in the UI pass.
    ui.Call(0x82FE39A0, {garage_header.render_runtime});
    ui.Call(0x82FECE38, {garage_header.presentation, clock});
  }
}

void ReleaseGarageHeader(Native& ui) {
  if (!garage_header.preloader) return;
  GarageHeaderVisibility(ui, false);
  const auto presentation = garage_header.presentation;
  if (garage_header.render_owner)
    ui.Call(0x82F2F828, {Read(ui.base, 0x833CAAC0), garage_header.render_owner, 0});
  ui.Call(0x82EB2C98, {garage_header.preloader + 0x50});
  ui.Call(0x824CEAB0, {garage_header.preloader + 0x50});
  ui.Call(0x82F27400, {garage_header.preloader});
  ui.Call(0x8242D1A0, {garage_header.preloader});
  {
    std::lock_guard lock(contracts_mutex);
    contracts.erase(presentation);
    stackers.erase(presentation);
    class_badges.erase(presentation);
  }
  REXLOG_INFO("FH1 garage header: released presentation={:08X}", presentation);
  garage_header = {};
}
uint32_t GridLayout() { return state.page == 2 ? 3u : 1u; }
uint32_t TileSlot(uint32_t logical) {
  constexpr uint32_t home_slots[] = {0, 1, 2, 4, 5, 6};
  return state.page == 0 && logical < std::size(home_slots)
      ? home_slots[logical] : logical;
}
std::string GridPrefix() {
  return "parent.Layer.Grid_Menu.Group.Grid_Menu_0" +
         std::to_string(GridLayout()) + ".Group.Button_";
}
void CaptureHomeTileGeometry(Native& ui) {
  if (!home_tile_geometry.empty()) return;
  const std::string original = "parent.Layer.Grid_Menu.Group.Grid_Menu_01.Group.Button_2";
  const std::string donor = "parent.Layer.Grid_Menu.Group.Grid_Menu_04.Group.Button_2";
  const auto target = ui.Node(original);
  const auto source = ui.Node(donor);
  if (!target || !source) return;
  const auto transform = [&](uint32_t node) {
    const auto instance = Read(ui.base, node + 4);
    const auto scalar = [&](const char* name, uint32_t offset) {
      const auto hash = ui.Call(0x82F354E0, {ui.String(name)});
      if (ui.Call(0x82FEF230, {node, hash, ui.scratch + 800}))
        return std::bit_cast<float>(Read(ui.base, ui.scratch + 800));
      return std::bit_cast<float>(Read(ui.base, instance + offset));
    };
    return TileTransform{scalar("position.x", 0xC), scalar("position.y", 0x10),
                         scalar("scale.x", 0x24), scalar("scale.y", 0x28)};
  };
  const auto copy = [&](uint32_t node, uint32_t from, bool keep_x = false) {
    if (!node || !from) return;
    const auto instance = Read(ui.base, node + 4);
    const auto donor_instance = Read(ui.base, from + 4);
    if (!instance || !donor_instance || Read(ui.base, instance + 4) != node ||
        Read(ui.base, donor_instance + 4) != from) return;
    const auto stock = transform(node);
    auto tall = transform(from);
    if (keep_x) tall.x = stock.x;
    home_tile_geometry.push_back({node, stock, tall});
  };
  // Grid 04's narrow central tile already spans both rows. Retain the
  // original Grid 01 x coordinate and copy its authored geometry, including
  // label anchors, underline and shadows; never stretch the text's parent.
  copy(target, source, true);
  for (const auto name : {"Orange_HL", "White_BG", "Button_Text", "Desc_Text",
                          "ShadowCorner_TopLeft", "ShadowCorner_TopRight",
                          "ShadowCorner_BotLeft", "ShadowCorner_BotRight",
                          "ShadowStraight_Top", "ShadowStraight_Bottom",
                          "ShadowStraight_Left", "ShadowStraight_Right"}) {
    const auto donors = Descendants(ui.base, source, Hash(name));
    if (donors.empty()) continue;
    for (const auto node : Descendants(ui.base, target, Hash(name)))
      copy(node, donors.front());
  }
  REXLOG_INFO("FH2 pause Home: copied {} native tall-tile transforms",
              home_tile_geometry.size());
}
void ApplyHomeTileGeometry(Native& ui) {
  if (GridLayout() != 1) return;
  CaptureHomeTileGeometry(ui);
  for (const auto& entry : home_tile_geometry) {
    const auto& value = state.page == 0 ? entry.tall : entry.original;
    const auto instance = Read(ui.base, entry.node + 4);
    if (!instance || Read(ui.base, instance + 4) != entry.node) continue;
    if (Read(ui.base, instance + 0xC) != std::bit_cast<uint32_t>(value.x) ||
        Read(ui.base, instance + 0x10) != std::bit_cast<uint32_t>(value.y))
      ui.Position(entry.node, value.x, value.y);
    if (Read(ui.base, instance + 0x24) != std::bit_cast<uint32_t>(value.scale_x) ||
        Read(ui.base, instance + 0x28) != std::bit_cast<uint32_t>(value.scale_y))
      ui.ScaleIn(state.engine, entry.node, value.scale_x, value.scale_y);
  }
}
void RestoreTileFocus(Native& ui) {
  REXLOG_INFO("FH2 pause focus: page={} layout={} selection={}", state.page, state.layout, state.selection);
  std::vector<uint32_t> buttons;
  {
    std::lock_guard lock(contracts_mutex);
    for (const auto& contract : contracts[state.engine])
      if (contract.type == "anthem_pause_menu_button" ||
          contract.type == "pause_menu_button") buttons.push_back(contract.node);
  }
  const auto tiles = Tiles();
  for (unsigned i = 0; i < 7; ++i) {
    const auto node = ui.Node(GridPrefix() + std::to_string(i));
    const auto contract = fh2_focus_binding::FindContract(
        node, buttons, [&](uint32_t n) { return Read(ui.base, n + 12); });
    ui.Event(contract, state.selection < tiles.size() && i == TileSlot(state.selection) &&
                              ActionAvailable(ui.base, tiles[state.selection].action)
                          ? fh2_grid_events::Focus(false) : fh2_grid_events::Blur(false));
  }
  // Native focus is queued. Reapply the copied shape after those events run.
  theme_frames = 4;
}
void BindTiles(Native& ui) {
  UpdateFooter(ui, true);
  if (state.page == 2) {
    using fh1_festival_travel::Destination;
    state.garage_available = fh1_festival_travel::Available(ui.ctx, ui.base, Destination::Garage);
    state.autoshow_available = fh1_festival_travel::Available(ui.ctx, ui.base, Destination::Autoshow);
  }
  // Fresh scenes have no destination availability cache until the native
  // checks above finish. Preserve a child-return selection until then.
  NormalizeSelection(ui.base);
  tile_label_styles.clear();
  tile_underlines.clear();
  festival_progress_nodes.clear();
  festival_next_nodes.clear();
  festival_progress_text.clear();
  festival_progress_frames = 0;
  theme_titles.clear();
  theme_descriptions.clear();
  theme_backgrounds.clear();
  theme_accents.clear();
  theme_panels.clear();
  theme_hidden.clear();
  theme_tabs.clear();
  const auto tiles = Tiles();
  // Cars uses FH2's authored large square and two stacked wide tiles.
  // Other tabs retain the existing first-page layout. Reapply a page event only
  // when its geometry changes, then restore selection after native default focus.
  const auto layout = GridLayout();
  if (state.layout != layout) {
    ui.Event(state.grid, fh2_grid_events::Immediate(layout - 1));
    state.layout = layout;
    state.restore_focus_ticks = 2;
  }
  const auto prefix = GridPrefix();
  CaptureHomeTileGeometry(ui);
  const auto font_source = ui.Node("parent.Layer.Grid_Menu.Group.Grid_Menu_01.Group.Button_2");
  const auto title_fonts = Descendants(ui.base, font_source, Hash("Button_Text"));
  const auto body_fonts = Descendants(ui.base, font_source, Hash("Desc_Text"));
  std::vector<uint32_t> button_contracts;
  {
    std::lock_guard lock(contracts_mutex);
    for (const auto& contract : contracts[state.engine])
      if (contract.type == "anthem_pause_menu_button" ||
          contract.type == "pause_menu_button")
        button_contracts.push_back(contract.node);
  }
  const auto contract_for = [&](uint32_t node) {
    return fh2_focus_binding::FindContract(
        node, button_contracts, [&](uint32_t n) { return Read(ui.base, n + 12); });
  };
  for (unsigned slot = 0; slot < 7; ++slot) {
    const auto button = ui.Node(prefix + std::to_string(slot));
    if (!button) continue;
    unsigned i = 0;
    while (i < tiles.size() && TileSlot(i) != slot) ++i;
    if (i >= tiles.size()) {
      ui.Opacity(button, 0);
      theme_hidden.push_back(button);
      ui.Event(contract_for(button), fh2_grid_events::Blur(false));
      continue;
    }
    const bool available = ActionAvailable(ui.base, tiles[i].action);
    for (const auto node : Descendants(ui.base, button, Hash("Orange_HL")))
      tile_underlines.push_back(node);
    const bool progress = state.page == 0 && tiles[i].action == 0x7111;
    const bool tall = state.page == 0 && i == 2;
    const bool square = state.page == 2 && i == 0;
    const float width = square ? 370.f :
        (state.page == 2 || slot < 2 || slot == 4 ? 369.f : 183.f);
    const float height = tall || square ? 370.f : 183.f;
    const float center_y = tall || (GridLayout() == 1 && slot == 4) ? 93.f : 0.f;
    const auto label_style = [&](uint32_t node, bool title) {
      const auto& fonts = title ? title_fonts : body_fonts;
      if (!fonts.empty()) ui.FontFrom(node, fonts.front());
      tile_label_styles.push_back({node, -width * .5f + 18.f,
          center_y + (title ? height * .5f - 18.f : -height * .5f + 18.f),
          width - 36.f, title});
    };
    ui.Opacity(button, available ? 100.f : 55.f);
    ui.Event(contract_for(button), available && i == state.selection
                                       ? fh2_grid_events::Focus(false)
                                       : fh2_grid_events::Blur(false));
    for (const auto material : Descendants(ui.base, button, Hash("Material294")))
      theme_accents.push_back(material);
    for (const auto name : {"Material84", "Material6374", "Material6436"})
      for (const auto material : Descendants(ui.base, button, Hash(name)))
        theme_backgrounds.push_back({material,
            available && i == state.selection ? 0x602342u : 0x292529u});
    for (const auto name : {"Button_Text2", "MessageCount", "STATUS_LABEL_BIG_ORANGE"})
      for (const auto node : Descendants(ui.base, button, Hash(name))) {
        const auto label = std::string(name);
        if (state.page == 0 && tiles[i].action == 0x7111 &&
            (label == "Button_Text2" || label == "MessageCount")) continue;
        if (state.page == 2 && i == 0 && label == "Button_Text2") {
          label_style(node, false);
          ui.Text(node, tiles[i].description);
          ui.Show(node, true);
          theme_descriptions.push_back(node);
          continue;
        }
        // These native infographic fields are reused by Home's live progress
        // tile. Clear their text as well as hiding them when another tab binds.
        if (label == "Button_Text2" || label == "MessageCount") ui.Text(node, "");
        theme_hidden.push_back(node);
        ui.Opacity(node, 0);
      }
    {
      // Locked-tile backers are opaque grey rectangles. They must stay hidden
      // on every adapted page so the FH1 brush edges have no rectangular frame.
      for (const auto name : {"PageIndicator", "Lock_USE_Small", "Locked_Tile_183x183",
                              "Locked_Tile_370x183", "Locked_Tile_372x372",
                              "Photomode", "MyProfile", "Shadow",
                              "ShadowCorner_TopLeft", "ShadowCorner_TopRight",
                              "ShadowCorner_BotLeft", "ShadowCorner_BotRight",
                              "ShadowStraight_Top", "ShadowStraight_Bottom",
                              "ShadowStraight_Left", "ShadowStraight_Right"})
        for (const auto node : Descendants(ui.base, button, Hash(name))) {
          theme_hidden.push_back(node);
          ui.Show(node, false);
          ui.Opacity(node, 0);
        }
      // Photo/Profile authored their shared paper backing at zero opacity.
      // Reuse that same backing on every tile instead of their opaque artwork.
      for (const auto name : {"White_BG", "White_BG2"})
        for (const auto node : Descendants(ui.base, button, Hash(name)))
          theme_panels.push_back(node);
    }
    for (const auto node : Descendants(ui.base, button, Hash("Button_Text"))) {
      ui.Text(node, tiles[i].label);
      label_style(node, true);
      theme_titles.push_back(node);
    }
    for (const auto node : Descendants(ui.base, button, Hash("Desc_Text"))) {
      ui.Text(node, tiles[i].description);
      if (!progress) label_style(node, false);
      theme_descriptions.push_back(node);
      if (state.page == 0 && tiles[i].action == 0x7111) festival_progress_nodes.push_back(node);
    }
    if (state.page == 0 && tiles[i].action == 0x7111) {
      // This authored wide tile has Button_Text2, not Desc_Text. Reuse its
      // secondary caption for the completion summary beside the native ring.
      // Both keep the original tile's transform/clip and rendering lifetime.
      const auto prepare = [&](uint32_t node, float x, float y, uint32_t vertical) {
        ui.Show(node, true);
        ui.Opacity(node, 100);
        if (!theme_descriptions.empty()) ui.FontFrom(node, theme_descriptions.front());
        ui.TextSize(node, 11.f);
        ui.TextAlignment(node, 0);
        ui.Position(node, x, y);
        const auto width = ui.Call(0x82F354E0, {ui.String("boxwidth")});
        ui.ctx.f1.f64 = 175.f / 1200.f;
        ui.Call(0x82F34DE0, {state.engine, node, width});
        // The authored counter's default width is only 20 px. Constant text
        // nodes can omit a boxwidth property; 82F2A8D0 caches it at text+74.
        const auto instance = Read(ui.base, node + 4);
        const auto text = instance ? Read(ui.base, instance) : 0;
        if (text && Read(ui.base, instance + 4) == node) {
          Write(ui.base, text + 0x70, std::bit_cast<uint32_t>(.10f));
          Write(ui.base, text + 0x74, std::bit_cast<uint32_t>(175.f / 1200.f));
          // 82F2AC00 converts the size property to this integer cache field.
          Write(ui.base, text + 0x80, 11);
          Write(ui.base, text + 0x7C, 1);
          Write(ui.base, text + 0xDC, 2);
          Write(ui.base, text + 0xC0, 0); // Native horzalign cache: left.
          Write(ui.base, text + 0xC4, vertical);
          GuestPtr(ui.base, text + 0x105)[0] = 1;
          GuestPtr(ui.base, text + 0x10C)[0] = 1;
        }
        ui.Text(node, "");
        theme_descriptions.push_back(node);
      };
      for (auto node : Descendants(ui.base, button, Hash("Button_Text2"))) {
        prepare(node, -168.f, 94.f, 0);
        festival_progress_nodes.push_back(node);
      }
      for (auto node : Descendants(ui.base, button, Hash("MessageCount"))) {
        ui.Text(node, "");
        ui.Show(node, false);
        theme_hidden.push_back(node);
      }
    }
  }
  BindPlayerIdentity(ui);
  UpdatePlayerIdentity(ui, true);
  // Keep the original measured-width tab controller, with FH1's title font
  // and palette. Divider/notification nodes remain owned layout anchors.
  constexpr const char* labels[] = {"HOME", "MAP", "CARS", "SETTINGS"};
  const auto enabled_pages = EnabledPages(ui.base);
  for (unsigned i = 0; i < 6; ++i) {
    const auto path = std::string("parent.Layer2.ANTH_TabControl_New.GROUP_ITEMS.Tab_") +
                      std::to_string(i + 1);
    const auto tab = ui.Node(path);
    ui.Opacity(tab, i < 4 ? (enabled_pages & (1u << i) ? 100.f : 45.f) : 0.f);
    if (i >= 4) {
      ui.Show(tab, false);
      theme_hidden.push_back(tab);
    }
    if (i < 4) {
      ui.Text(ui.Node(path + ".Text_Tab"), labels[i]);
      for (const auto* name : {"TabDivider", "Notification"}) {
        const auto decoration = ui.Node(path + "." + name);
        ui.Show(decoration, false);
        theme_hidden.push_back(decoration);
      }
      ui.Event(contract_for(tab), i == state.page ? fh2_grid_events::Select()
                                                : fh2_grid_events::Unselect());
      for (const auto material : Descendants(ui.base, tab, Hash("Material252")))
        theme_tabs.push_back({material, i == state.page ? 0xFF007Au : 0x292529u});
    }
  }
  REXLOG_INFO("FH2 pause scene: page={} tiles={}", state.page, tiles.size());
  if (state.page == 0)
    REXLOG_INFO("FH2 pause festival progress: bound {} description nodes", festival_progress_nodes.size());
  BindFestivalGauge(ui);
  UpdateFestivalProgress(ui, true);
  theme_frames = 4;
}
void StyleTileLabels(Native& ui) {
  for (const auto& label : tile_label_styles) {
    ui.TextSize(label.node, label.title ? 22.f : 11.f);
    ui.Position(label.node, label.x, label.y);
    ui.TextAlignment(label.node, 0);
    const auto scalar = [&](const char* name, float value) {
      const auto hash = ui.Call(0x82F354E0, {ui.String(name)});
      ui.ctx.f1.f64 = value;
      ui.Call(0x82F34DE0, {state.engine, label.node, hash});
    };
    const auto integer = [&](const char* name, uint32_t value) {
      const auto hash = ui.Call(0x82F354E0, {ui.String(name)});
      const auto property = ui.Call(0x82FEF130, {label.node, hash});
      if (property) ui.Call(0x82FF2748, {state.access, label.node, property, value});
    };
    // Native text boxes use viewport-normalized dimensions. The old Controls
    // title's .03856 height forced its two lines to shrink despite size=22.
    scalar("boxwidth", label.width / 1200.f);
    scalar("boxheight", label.title ? .10f : .06f);
    integer("texttype", 1); // Native boxed fit, retaining authored wrapping.
    integer("vertalign", label.title ? 0u : 2u);
    // Constant FBF fields may have no writable property. Mirror the native
    // text cache as well, so every tile uses the same fit box and baseline.
    const auto instance = Read(ui.base, label.node + 4);
    const auto text = instance ? Read(ui.base, instance) : 0;
    if (text && Read(ui.base, instance + 4) == label.node) {
      Write(ui.base, text + 0x70,
            std::bit_cast<uint32_t>(label.title ? .10f : .06f));
      Write(ui.base, text + 0x74, std::bit_cast<uint32_t>(label.width / 1200.f));
      Write(ui.base, text + 0x80, label.title ? 22 : 11);
      Write(ui.base, text + 0x7C, 1);
      Write(ui.base, text + 0xC0, 0);
      Write(ui.base, text + 0xC4, label.title ? 0 : 2);
      Write(ui.base, text + 0xDC, 2); // Native shrink-to-fit, never ellipsis.
      GuestPtr(ui.base, text + 0x104)[0] = 1; // Authored multi-line labels.
      GuestPtr(ui.base, text + 0x105)[0] = 1;
      GuestPtr(ui.base, text + 0x10C)[0] = 1;
    }
  }
}
void MaintainTileSelectionVisuals(Native& ui) {
  // The pink paper already marks selection. Native focus events can restore
  // Orange_HL, so suppress its visibility and opacity after those events run.
  for (const auto node : tile_underlines) {
    ui.Show(node, false);
    ui.Opacity(node, 0);
  }
}
void Theme(Native& ui) {
  ApplyHomeTileGeometry(ui);
  StyleTileLabels(ui);
  BindFestivalGauge(ui);
  // Presentation events apply their authored colors asynchronously. Apply the
  // FH1 palette after those events, retaining the original geometry/animation.
  for (const auto node : theme_titles) {
    ui.Color(node, 0xFFFFFF, true);
    ui.Opacity(node, 100);
  }
  for (const auto node : theme_descriptions) {
    ui.Color(node, 0xD9D9D9, true);
    ui.Opacity(node, 100);
  }
  for (const auto node : theme_panels) {
    ui.Show(node, true);
    ui.Opacity(node, 100);
  }
  for (const auto [node, color] : theme_backgrounds) ui.Color(node, color);
  for (const auto node : theme_accents) ui.Color(node, 0xFF007A);
  for (const auto [node, color] : theme_tabs) ui.Color(node, color);
  for (const auto node : theme_hidden) ui.Opacity(node, 0);
}
void HeaderAndTabs(Native& ui) {
  const std::string tabs="parent.Layer2.ANTH_TabControl_New";
  unsigned styled_labels = 0;
  std::array<uint32_t, 4> tab_labels{}, tab_backgrounds{};
  // Align the text to its own native background center, rather than using
  // font-dependent bottom alignment. The stacker continues to own horizontal
  // placement and sizes each background from the actual FH1 text width.
  for (unsigned i = 1; i <= 4; ++i) {
    const auto path = tabs + ".GROUP_ITEMS.Tab_" + std::to_string(i);
    const auto text_node = ui.Node(path + ".Text_Tab");
    const auto background = ui.Node(path + ".TabBg");
    if (!text_node || !background) continue;
    tab_labels[i - 1] = text_node;
    tab_backgrounds[i - 1] = background;
    if (!theme_titles.empty()) ui.FontFrom(text_node, theme_titles.front());
    ui.TextSize(text_node, 20.f);
    ui.TextAlignment(text_node, 1);
    const auto vertical_hash = ui.Call(0x82F354E0, {ui.String("vertalign")});
    const auto vertical = ui.Call(0x82FEF130, {text_node, vertical_hash});
    if (vertical && (Read(ui.base, vertical) & 0x1C) == 4)
      ui.Call(0x82FF2748, {state.access, text_node, vertical, 1});
    const auto instance = Read(ui.base, text_node + 4);
    const auto bg_instance = Read(ui.base, background + 4);
    if (!instance || !bg_instance || Read(ui.base, instance + 4) != text_node ||
        Read(ui.base, bg_instance + 4) != background) continue;
    const auto text = Read(ui.base, instance);
    if (text) {
      // Static FBF text can omit these scalar properties. Mirror the native
      // text cache and invalidate geometry so its measured extent is current.
      Write(ui.base, text + 0x80, 20);
      Write(ui.base, text + 0xC0, 1);
      Write(ui.base, text + 0xC4, 1);
      GuestPtr(ui.base, text + 0x105)[0] = 1;
      GuestPtr(ui.base, text + 0x10C)[0] = 1;
    }
    ui.Position(text_node,
                std::bit_cast<float>(Read(ui.base, instance + 0xC)),
                std::bit_cast<float>(Read(ui.base, bg_instance + 0x10)));
    ui.Color(text_node, 0xFFFFFF, true);
    ++styled_labels;
  }
  for (const auto* suffix : {".GROUP_LB", ".Component_RB"}) {
    const auto bumper = ui.Node(tabs + suffix);
    if (!bumper) continue;
    for (const auto material : Descendants(ui.base, bumper, Hash("Material294")))
      ui.Color(material, 0x292529);
    for (const auto divider : Descendants(ui.base, bumper, Hash("TabDivider"))) {
      ui.Show(divider, false);
      ui.Opacity(divider, 0);
    }
  }
  std::vector<uint32_t> layouts;
  {
    std::lock_guard lock(contracts_mutex);
    layouts=stackers[state.engine];
  }
  for(auto controller:layouts) {
    auto result=fh2_tab_layout::UseFourTabs(ui,controller,
        ui.Node(tabs+".Component_RB"),
        ui.Node(tabs+".GROUP_ITEMS.Tab_4.TabDivider"),
        ui.Node(tabs+".GROUP_ITEMS.Tab_6.TabDivider"));
    if(result==fh2_tab_layout::Result::Applied)
      REXLOG_INFO("FH2 pause: native tab stacker now uses four tabs");
    if (result != fh2_tab_layout::Result::NotReady) {
      // Font/size changes alter native measured widths even after compaction.
      // 828D1E08 runs Measure then Place; it also honors the stacker's freeze flag.
      ui.Call(0x828D1E08, {controller});
      std::array<fh2_tab_layout::TabMetrics, 4> metrics{};
      std::array<uint32_t, 4> native_alignments{};
      const auto alignment_hash = ui.Call(0x82F354E0, {ui.String("horzalign")});
      for (unsigned i = 0; i < 4; ++i) {
        const auto binding = Read(ui.base, controller + 0x70 + 3 * i * 0x10);
        if (!binding) continue;
        Write(ui.base, ui.scratch + 800, 0);
        // Use the exact query and zero default in native Place, which can
        // differ from the centered static text-renderer cache.
        ui.Call(0x82F35AD8, {binding, alignment_hash, ui.scratch + 800});
        native_alignments[i] = Read(ui.base, ui.scratch + 800);
      }
      const bool centered = fh2_tab_layout::CenterLabels(
          ui, controller, tab_labels, tab_backgrounds, metrics, native_alignments);
      if (theme_frames == 1) {
        std::array<float, 4> widths{};
        for (unsigned i = 0; i < widths.size(); ++i) {
          const auto slot = 3 * i;
          const auto delta = [&](unsigned n) {
            return std::bit_cast<float>(Read(ui.base, controller + 0x1B0 + n * 4));
          };
          widths[i] = delta(slot + 1) - delta(slot) +
              std::bit_cast<float>(Read(ui.base, controller + 0x74 + slot * 0x10));
        }
        REXLOG_INFO("FH1 pause tabs: styled={} measured widths={:.2f}/{:.2f}/{:.2f}/{:.2f} alignment-adjust={}",
                    styled_labels, widths[0], widths[1], widths[2], widths[3],
                    unsigned(GuestPtr(ui.base, controller)[0x378]));
        if (!centered) {
          REXLOG_WARN("FH1 pause tabs: center binding rejected; first relative start={} end={} mode={}",
                      Read(ui.base, controller + 0x218), Read(ui.base, controller + 0x21C),
                      Read(ui.base, controller + 0x220));
        } else {
          for (unsigned i = 0; i < 4; ++i) {
            const auto text = Read(ui.base, tab_labels[i] + 4);
            const auto background = Read(ui.base, tab_backgrounds[i] + 4);
            const auto x = [&](uint32_t instance, uint32_t offset) {
              return instance ? std::bit_cast<float>(Read(ui.base, instance + offset)) : 0.f;
            };
            REXLOG_INFO("FH1 pause tab {}: center={:.2f} widths={:.2f}/{:.2f} padding={:.2f} localX={:.2f}/{:.2f} worldX={:.2f}/{:.2f}",
                        i, metrics[i].text_x, metrics[i].text_width,
                        metrics[i].background_width,
                        (metrics[i].background_width - metrics[i].text_width) * .5f,
                        x(text, 0xC), x(background, 0xC), x(text, 0xAC), x(background, 0xAC));
          }
        }
      }
      state.tab_stacker = controller;
      state.right_bumper = ui.Node(tabs + ".Component_RB");
      state.last_divider = ui.Node(tabs + ".GROUP_ITEMS.Tab_4.TabDivider");
    }
  }
}

void Resume(PPCContext& ctx, uint8_t* base) {
  const auto screen = state.screen;
  return_page.reset();
  return_selection.reset();
  if (fh1_pause_actions::Resume(ctx, base, screen) && state.screen == screen)
    state.visible = false;
}
void ResetCarPosition(PPCContext& ctx, uint8_t* base) {
  // Recovery revalidates the native gates before queuing USER0's request.
  if (NativeActionAvailable(base, 2) && Read(base, state.screen + 0x1DC) &&
      fh1_car_recovery::Request(ctx, base))
    Resume(ctx, base);
}
uint32_t Neighbor(uint8_t* base, uint16_t direction) {
  struct Point { float x, y; };
  constexpr Point home_positions[] = {
      {1, .5f}, {1, 1.5f}, {2.5f, 1.f},
      {4, .5f}, {3.5f, 1.5f}, {4.5f, 1.5f}};
  constexpr Point settings_positions[] = {
      {1, .5f}, {1, 1.5f}, {2.5f, .5f}, {2.5f, 1.5f},
      {4, .5f}, {3.5f, 1.5f}};
  if (state.page == 1) return 0;
  NormalizeSelection(base);
  const auto tiles = Tiles();
  constexpr Point cars_positions[] = {{1.f, 1.f}, {3.f, .5f}, {3.f, 1.5f}};
  const auto positions = state.page == 2 ? cars_positions :
                         state.page == 3 ? settings_positions : home_positions;
  const auto origin = positions[state.selection];
  const float dx = direction & 4 ? -1.f : direction & 8 ? 1.f : 0.f;
  const float dy = direction & 1 ? -1.f : direction & 2 ? 1.f : 0.f;
  auto best = state.selection;
  float score = 10000;
  for (uint32_t i = 0; i < tiles.size(); ++i) {
    if (!ActionAvailable(base, tiles[i].action)) continue;
    const float x = positions[i].x - origin.x;
    const float y = positions[i].y - origin.y;
    const float forward = x * dx + y * dy;
    if (forward <= 0) continue;
    const float cross = x * dy - y * dx;
    const float candidate = forward * forward + cross * cross * 3;
    if (candidate < score) {
      score = candidate;
      best = i;
    }
  }
  return best;
}
void Activate(PPCContext& ctx, uint8_t* base) {
  const auto tiles = Tiles();
  if (state.selection >= tiles.size()) return;
  const auto action = tiles[state.selection].action;
  if (!ActionAvailable(base, action)) return;
  if (action == 0x7110) {
    Native ui(ctx, base);
    if (ui.scratch) {
      state.page = 3;
      state.selection = 0;
      BindTiles(ui);
    }
    return;
  }
  if (action == fh1_pause_actions::kResume) {
    Resume(ctx, base);
    return;
  }
  const auto screen = state.screen;
  const auto previous_return_page = return_page;
  const auto previous_return_selection = return_selection;
  return_page = state.page; // Set before a synchronous native transition.
  return_selection = state.selection;
  bool dispatched = false;
  if (action == 0x7111) {
    dispatched = fh1_pause_actions::OpenProgress(ctx, base, screen);
  } else if (action == 0x7108) {
    dispatched = offline_cars::Open(ctx, base, screen);
  } else if (action == 0x7112 || action == 0x7113) {
    dispatched = fh1_festival_travel::Open(ctx, base, screen,
        action == 0x7112 ? fh1_festival_travel::Destination::Garage
                         : fh1_festival_travel::Destination::Autoshow);
  } else if (action == 0x710C) {
    dispatched = fh1_pause_actions::OpenAudio(ctx, base, screen);
  } else if (action == 0x710D) {
    dispatched = fh1_pause_actions::OpenControls(ctx, base, screen);
  } else if (action == 0x710E) {
    dispatched = fh1_pause_actions::OpenHud(ctx, base, screen);
  } else if (action == 0x710F) {
    dispatched = fh1_pause_actions::OpenVideo(ctx, base, screen);
  } else if (action == 0x7104) {
    Native ui(ctx, base);
    if (ui.scratch) {
      const auto params = ui.Call(0x824265C8, {0x88});
      if (params) {
        ui.Call(0x825A5640, {params, 0});
        if (const auto title = ui.TextRef("DIFFICULTY")) {
          ui.Call(0x82471820, {params + 0xC, title});
          ui.Call(0x824CEAB0, {title});
        }
        Write(base, ui.scratch, params);
        ui.Call(0x82675B00, {screen, 5, ui.scratch, 0});
        dispatched = true;
      }
    }
  } else if (action == 0x7000) {
    Native ui(ctx, base);
    if (ui.scratch) {
      ui.Call(0x827EBEE0, {screen, 2, 0, action});
      // Graphics calls Page(false) after successfully creating its child. It
      // also owns failure cleanup, so do not preemptively disable this parent.
      dispatched = state.screen != screen || !state.visible;
    }
  } else {
    dispatched = fh1_pause_actions::Dispatch(ctx, base, screen, action);
  }
  if (!dispatched) {
    return_page = previous_return_page;
    return_selection = previous_return_selection;
    return;
  }
  // Some native children (notably Difficulty) retain their parent's
  // presentation. Explicitly hide ours once the child was created successfully.
  if (state.screen == screen && state.visible) Page(ctx, base, screen, false);
}
} // namespace

bool Owns(uint32_t screen) {
  return screen && state.screen == screen;
}
bool Input(PPCContext& ctx, uint8_t* base) {
  if (!Owns(ctx.r3.u32)) return false;
  if (!state.visible) return true;
  Native ui(ctx, base);
  const auto held = HeldButtons(ui);
  if (!held) return true;
  state.blocked &= *held;
  const auto pressed = uint16_t(*held & ~state.previous & ~state.blocked);
  state.previous = *held;
  if (pressed & (0x2000 | 0x10)) {
    Resume(ctx, base);
    GarageHeaderVisibility(ui, false);
    return true;
  }
  if (pressed & 0x40) {
    ResetCarPosition(ctx, base);
    GarageHeaderVisibility(ui, state.visible);
    return true;
  }
  if (pressed & 0x300) {
    const auto next = fh2_navigation::Navigate(
        state.page, (pressed & 0x100) ? -1 : 1, EnabledPages(base));
    if (next != state.page) {
      state.page = next;
      state.selection = FirstAvailableSelection(base);
      BindTiles(ui);
    }
    return true;
  }
  if (pressed & 0xF) {
    state.selection = Neighbor(base, pressed & 0xF);
    REXLOG_INFO("FH2 pause scene: selection={}", state.selection);
    BindTiles(ui);
    return true;
  }
  if (pressed & 0x1000) {
    Activate(ctx, base);
    GarageHeaderVisibility(ui, state.visible);
  }
  return true;
}
bool Page(PPCContext& ctx, uint8_t* base, uint32_t screen, bool show) {
  if (!Owns(screen)) return false;
  state.visible = show;
  Native ui(ctx, base);
  if (ui.scratch) {
    ui.Call(0x82F1EE68, {screen, uint32_t(show), 0});
    GarageHeaderVisibility(ui, show);
    if (show) {
      UpdateFooter(ui, true);
      BlockHeldInput(ui);
      // Native SHOW can restore the authored first-button focus after a fresh
      // child return. Restore our selection after that presentation event pass.
      state.restore_focus_ticks = 2;
    }
  } else if (show) {
    state.previous = state.blocked = 0xFFFF;
  }
  return true;
}
void Destroy(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  if (!Owns(screen)) return;
  Native ui(ctx, base);
  if (ui.scratch) ReleaseGarageHeader(ui);
  player_identity = {};
  tile_label_styles.clear();
  tile_underlines.clear();
  home_tile_geometry.clear();
  festival_progress_nodes.clear();
  festival_gauge = {};
  festival_next_nodes.clear();
  festival_progress_text.clear();
  festival_progress_frames = 0;
  for (const auto [node, wrapper] : event_wrappers)
    REX_KERNEL_MEMORY()->SystemHeapFree(wrapper);
  event_wrappers.clear();
  theme_titles.clear();
  theme_descriptions.clear();
  theme_backgrounds.clear();
  theme_accents.clear();
  theme_panels.clear();
  theme_hidden.clear();
  theme_frames = 0;
  theme_tabs.clear();
  std::lock_guard lock(contracts_mutex);
  contracts.erase(state.engine);
  progress_controllers.erase(state.engine);
  stackers.erase(state.engine);class_badges.erase(state.engine);
  state = {};
}
} // namespace forza::fh2_pause_scene

extern "C" REX_FUNC(sub_82F39B08) {
  using namespace forza::fh2_pause_scene;
  const auto object = ctx.r5.u32;
  const auto pointer = Read(base, object + 20) >= 16 ? Read(base, object) : object;
  std::string type;
  for (unsigned i = 0; i < 80; ++i) {
    const auto character = forza::GuestPtr(base, pointer)[i];
    if (!character) break;
    type += char(character);
  }
  const auto engine=Read(base,ctx.r4.u32+8);
  const auto progress_node = Read(base, ctx.r4.u32 + 12);
  if (type == "gridmenu" || type == "anth_tabcontrol" ||
      type == "anthem_pause_menu_button" || type == "pause_menu_button") {
    const auto wrapper = ctx.r4.u32;
    std::lock_guard lock(contracts_mutex);
    contracts[Read(base, wrapper + 8)].push_back(
        {Read(base, wrapper + 12), Read(base, wrapper + 16), type});
  }
  __imp__sub_82F39B08(ctx, base);
  const auto progress_owner = ctx.r3.u32 && progress_node &&
      Read(base, ctx.r3.u32) == 0x8207ED44 ? Read(base, progress_node + 12) : 0;
  if (progress_owner && Read(base, progress_owner) == Hash("RoadtripProgress")) {
    std::lock_guard lock(contracts_mutex);
    progress_controllers[engine].push_back({progress_owner, ctx.r3.u32});
    REXLOG_INFO("FH1 festival gauge: captured native controller={:08X} contract={:08X} owner={:08X}",
                ctx.r3.u32, progress_node, progress_owner);
  }
  if(ctx.r3.u32 && (type=="super_stacker"||type=="class_badge")) {
    std::lock_guard lock(contracts_mutex);
    (type=="super_stacker"?stackers:class_badges)[engine].push_back(ctx.r3.u32);
  }
}
extern "C" REX_FUNC(sub_828BAF48) {
  using namespace forza::fh2_pause_scene;
  const auto controller = ctx.r3.u32;
  if (controller && controller == festival_gauge.controller &&
      state.screen && state.visible && state.page == 0 &&
      Read(base, controller) == 0x8207ED44 &&
      Read(base, controller + 0xAC) == state.engine) {
    // Only this transplanted gauge reads FH1 career XP. All other native
    // progress bars retain their existing bindings and update behavior.
    ctx.r3.u64 = controller + 0xA4;
    ctx.f1.f64 = festival_gauge.fraction;
    rex::runtime::ResolveIndirectFunction(0x82F36650)(ctx, base);
    return;
  }
  __imp__sub_828BAF48(ctx, base);
}
extern "C" REX_FUNC(sub_827DFB18) {
  using namespace forza::fh2_pause_scene;
  const auto screen = ctx.r3.u32;
  const auto engine = Read(base, screen + 8);
  uint32_t grid = 0;
  {
    std::lock_guard lock(contracts_mutex);
    const auto entry = contracts.find(engine);
    if (entry != contracts.end())
      for (const auto& contract : entry->second)
        if (contract.type == "gridmenu") grid = contract.node;
  }
  if (!grid) {
    __imp__sub_827DFB18(ctx, base);
    return;
  }
  state = {};
  state.screen = screen;
  state.engine = engine;
  state.page = std::min(return_page.value_or(0), 3u);
  state.selection = std::min(return_selection.value_or(0),
                             uint32_t(Tiles().size() - 1));
  state.grid = grid;
  state.previous = state.blocked = 0xFFFF;
  return_page.reset();
  return_selection.reset();
  Native ui(ctx, base);
  if (!ui.scratch) return;
  state.access = ui.Call(Read(base, Read(base, engine) + 0x1C), {engine});
  // Claim the screen before building its model: its final MenuBind call must
  // bypass the legacy list while still registering this graphics-menu owner.
  if (!forza::fh1_pause_actions::InitModel(ctx, base, screen)) {
    REXLOG_WARN("FH2 pause scene: native pause model initialization rejected");
    return;
  }
  if (!(EnabledPages(base) & (1u << state.page))) {
    state.page = 0;
    state.selection = 0;
  }
  BindTiles(ui);
  BlockHeldInput(ui);
  StartGarageHeader(ui);
}
extern "C" REX_FUNC(sub_827D1578) {
  if (forza::fh2_pause_scene::Owns(ctx.r3.u32)) return;
  __imp__sub_827D1578(ctx, base);
}
extern "C" REX_FUNC(sub_827DAB28) {
  const auto screen = ctx.r3.u32;
  __imp__sub_827DAB28(ctx, base);
  forza::fh2_pause_scene::Page(ctx, base, screen, true);
}
extern "C" REX_FUNC(sub_827DE0D0) {
  using namespace forza::fh2_pause_scene;
  const auto screen = ctx.r3.u32;
  __imp__sub_827DE0D0(ctx, base);
  if (!Owns(screen)) return;
  Native ui(ctx, base);
  if (ui.scratch) UpdateGarageHeader(ui);
  if (ui.scratch) UpdateFestivalProgress(ui);
  if (ui.scratch) UpdatePlayerIdentity(ui);
  if (ui.scratch) UpdateFooter(ui);
  if (theme_frames && ui.scratch) {
    Theme(ui);
    HeaderAndTabs(ui);
    if (theme_frames == 1 && state.tab_stacker)
      forza::fh2_tab_layout::AnchorRightBumper(ui, state.tab_stacker,
                                      state.right_bumper, state.last_divider);
    --theme_frames;
  }

  if (ui.scratch && state.visible && state.restore_focus_ticks &&
      --state.restore_focus_ticks == 0) RestoreTileFocus(ui);
  if (ui.scratch && state.visible) MaintainTileSelectionVisuals(ui);

  if (const auto held = HeldButtons(ui)) {
    // Native input dispatch need not report releases. Clear only released bits
    // here; never consume a new press before the actual input callback sees it.
    state.previous &= *held;
    state.blocked &= *held;
  }
}
#else
namespace forza::fh2_pause_scene {
bool Owns(uint32_t) { return false; }
bool Input(PPCContext&, uint8_t*) { return false; }
bool Page(PPCContext&, uint8_t*, uint32_t, bool) { return false; }
void Destroy(PPCContext&, uint8_t*, uint32_t) {}
} // namespace forza::fh2_pause_scene
#endif
