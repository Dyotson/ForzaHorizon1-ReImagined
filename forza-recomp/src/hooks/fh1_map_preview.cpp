// Display FH1's existing HUD map target through the original H_MAP material.
// The donor initializes its binding only: it never registers or renders a map.
// tu_address_map: TU4 addresses
#include "../platform/fh1_map_preview.h"

#if FORZA_TU4 && defined(FORZA_FH2_PAUSE_SCENE)
#include "../platform/guest_memory.h"
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

extern "C" REX_FUNC(__imp__sub_826E22A8);

namespace forza::fh1_map_preview {
namespace {
std::atomic<uint32_t> requested_hud_record{0};
std::atomic<uint64_t> rendered_player_pixel{0};
std::atomic<uint32_t> rendered_player_size{0};
uint32_t Read(uint8_t* base, uint32_t address) {
  const auto* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}
void Write(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = GuestPtr(base, address);
  for (int i = 3; i >= 0; --i) { p[i] = uint8_t(value); value >>= 8; }
}
float Float(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(Read(base, address));
}
uint32_t Hash(const char* text) {
  uint32_t result = 0;
  while (*text) result = uint8_t(*text++) + result * 65599;
  return result * 65599;
}
struct Preview {
  uint32_t screen = 0, preloader = 0, presentation = 0, root = 0;
  uint32_t owner = 0, runtime = 0, render_root = 0;
  uint32_t group = 0, rectangle = 0, controller = 0;
  uint32_t caption = 0, paper = 0, tip = 0, bar = 0;
  uint32_t display = 0, display_material = 0;
  uint32_t hud_profile_ref = 0;
  bool hud_resources_owned = false;
  std::vector<uint32_t> display_ancestors;
  bool mirrored = false;
  std::array<uint32_t, 7> mirror_signature{};
  bool caption_font = false;
  unsigned frames = 0;
  bool ready = false, visible = false, positioned = false;
  std::vector<uint32_t> hidden;
} preview;

class Native {
public:
  PPCContext ctx;
  uint8_t* base;
  uint32_t scratch;
  Native(const PPCContext& context, uint8_t* memory)
      : ctx(context), base(memory),
        scratch(REX_KERNEL_MEMORY()->SystemHeapAlloc(1024)) {}
  ~Native() { if (scratch) REX_KERNEL_MEMORY()->SystemHeapFree(scratch); }
  uint32_t Call(uint32_t address, std::initializer_list<uint32_t> args = {}) {
    unsigned index = 0;
    for (auto value : args) switch (index++) {
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
  uint32_t Virtual(uint32_t object, uint32_t offset,
                   std::initializer_list<uint32_t> args) {
    return object ? Call(Read(base, Read(base, object) + offset), args) : 0;
  }
  uint32_t String(const char* value) {
    const std::string text(value);
    if (text.size() >= 256) return 0;
    std::copy(text.begin(), text.end(), GuestPtr(base, scratch));
    GuestPtr(base, scratch + text.size())[0] = 0;
    return scratch;
  }
  uint32_t Find(uint32_t, uint32_t root, const char* path) {
    // v48 gives the raw scene root, not a screen's UIContract node. Native
    // "parent.*" queries are contract-relative; walk the serialized hierarchy.
    std::string names(path);
    if (names.starts_with("parent.")) names.erase(0, 7);
    auto separator = names.find('.');
    const auto first = names.substr(0, separator);
    uint32_t found = 0;
    std::vector<uint32_t> pending{root};
    unsigned visited = 0;
    while (!pending.empty() && visited++ < 10000) {
      const auto node = pending.back(); pending.pop_back();
      if (!node) continue;
      if (Read(base, node) == Hash(first.c_str())) { found = node; break; }
      unsigned count = 0;
      for (auto child = Read(base, node + 20); child && count++ < 1000;
           child = Read(base, child + 16)) pending.push_back(child);
    }
    while (found && separator != std::string::npos) {
      names.erase(0, separator + 1);
      separator = names.find('.');
      const auto key = Hash(names.substr(0, separator).c_str());
      auto child = Read(base, found + 20);
      unsigned count = 0;
      while (child && count++ < 1000 && Read(base, child) != key)
        child = Read(base, child + 16);
      found = count > 1000 ? 0 : child;
    }
    return found;
  }
  void Text(uint32_t node, const std::string& text) {
    if (!node || text.size() > 120) return;
    auto* output = GuestPtr(base, scratch + 32);
    for (size_t i = 0; i < text.size(); ++i) {
      output[i * 2] = 0; output[i * 2 + 1] = uint8_t(text[i]);
    }
    output[text.size() * 2] = output[text.size() * 2 + 1] = 0;
    const auto object = Call(0x824265C8, {16});
    if (!object) return;
    Write(base, object, 0x82001674);
    for (unsigned offset = 4; offset < 16; offset += 4) Write(base, object + offset, 0);
    Call(0x82D6DE20, {object, scratch + 32, uint32_t(text.size())});
    Call(0x82555B90, {scratch, object});
    Write(base, scratch + 480, 0x820307A4);
    Write(base, scratch + 484, node);
    Write(base, scratch + 488, preview.presentation);
    Call(0x82F3A728, {scratch + 480, scratch});
    Call(0x824CEAB0, {scratch});
    Call(0x82F37BE0, {scratch + 480, 0xFFFFFF});
    const auto hash = Call(0x82F354E0, {String("size")});
    ctx.f1.f64 = 22.f;
    Call(0x82F34DE0, {preview.presentation, node, hash});
  }
  void Color(uint32_t node, uint32_t rgb) {
    if (!node) return;
    const auto instance = Read(base, node + 4);
    const auto material = instance ? Read(base, instance) : 0;
    if (material) {
      for (unsigned channel = 0; channel < 3; ++channel) {
        const float current = Float(base, material + 0x70 + channel * 4);
        if (!std::isfinite(current) || current < 0) continue;
        const float value = float((rgb >> (16 - channel * 8)) & 255) / 255.f;
        Write(base, material + 0x70 + channel * 4, std::bit_cast<uint32_t>(value));
        Write(base, material + 0xA0 + channel * 4, std::bit_cast<uint32_t>(value));
      }
    }
    Write(base, scratch + 480, 0x820307A4);
    Write(base, scratch + 484, node);
    Write(base, scratch + 488, preview.presentation);
    Call(0x82F37C40, {scratch + 480, rgb});
  }
  void Dirty(uint32_t node) {
    Write(base, scratch + 512, node);
    const auto dirty = Virtual(preview.presentation, 0x7C, {preview.presentation});
    if (dirty) Call(0x82FE57F0, {dirty, scratch + 512});
  }
  void Show(uint32_t node, bool show) {
    if (!node) return;
    Write(base, scratch + 480, 0x820307A4);
    Write(base, scratch + 484, node);
    Write(base, scratch + 488, preview.presentation);
    Call(0x82F35B48, {scratch + 480, uint32_t(show)});
    Dirty(node);
  }
  void Scalar(uint32_t node, const char* property, uint32_t instance_offset,
              float value) {
    const auto instance = Read(base, node + 4);
    if (!instance || Read(base, instance + 4) != node) return;
    const auto hash = Call(0x82F354E0, {String(property)});
    if (Call(0x82FEF130, {node, hash})) {
      ctx.f1.f64 = value;
      Call(0x82F34DE0, {preview.presentation, node, hash});
    }
    // This donor's animation has already been evaluated this frame. Match
    // the native instance setter as well, so authored values cannot leave the
    // cached transform at its original authored location.
    Write(base, instance + instance_offset, std::bit_cast<uint32_t>(value));
    GuestPtr(base, instance + 0xFB)[0] |= 0x20;
    Dirty(node);
  }
  void Opacity(uint32_t node) {
    const auto instance = node ? Read(base, node + 4) : 0;
    if (!instance || Read(base, instance + 4) != node) return;
    const auto hash = Call(0x82F354E0, {String("opacity")});
    if (Call(0x82FEF130, {node, hash})) {
      ctx.f1.f64 = 100.f;
      Call(0x82F34DE0, {preview.presentation, node, hash});
    }
    Write(base, instance + 0xE0, std::bit_cast<uint32_t>(1.f));
    GuestPtr(base, instance + 0xFB)[0] |= 0x80;
    Dirty(node);
  }
  void CaptionFont() {
    if (preview.caption_font) return;
    const auto parent = Read(base, preview.screen + 8);
    const auto parent_access = Virtual(parent, 0x1C, {parent});
    const auto root = Virtual(parent_access, 0x48, {parent_access});
    const auto button = Find(parent_access, root,
        "parent.Layer.Grid_Menu.Group.Grid_Menu_01.Group.Button_0");
    const auto source = button ? Find(parent_access, button, "Button_Text") : 0;
    if (!source) return;
    const auto hash = Call(0x82F354E0, {String("font")});
    const auto target_property = Call(0x82FEF130, {preview.caption, hash});
    const auto source_property = Call(0x82FEF130, {source, hash});
    if (!target_property || !source_property ||
        (Read(base, target_property) & 0x1C) != 0x14 ||
        (Read(base, source_property) & 0x1C) != 0x14) return;
    const auto access = Virtual(preview.presentation, 0x1C, {preview.presentation});
    Call(0x82FF2748, {access, preview.caption, target_property,
                     Read(base, source_property + 4)});
    preview.caption_font = true;
    REXLOG_INFO("FH1 map caption: copied native pause title font from={:08X}", source);
  }
};

bool Descendant(uint8_t* base, uint32_t node, uint32_t ancestor) {
  for (unsigned depth = 0; node && depth < 100; ++depth) {
    if (node == ancestor) return true;
    node = Read(base, node + 12);
  }
  return false;
}
void Drain(Native& ui, uint32_t manager) {
  if (!manager || !Read(ui.base, manager + 0x27C)) return;
  ui.Call(0x82FE3F28, {manager + 0x284});
  while (const auto count = Read(ui.base, manager + 0x27C)) {
    const auto pair = Read(ui.base, manager + 0x274) + (count - 1) * 8;
    const auto presentation = Read(ui.base, pair);
    const auto renderer = Read(ui.base, pair + 4);
    Write(ui.base, manager + 0x27C, count - 1);
    if (ui.Virtual(presentation, 8, {presentation})) {
      ui.Call(0x82FF2DD8, {manager + 0x28, presentation});
      const auto adapter = Read(ui.base, manager + 0x20C);
      ui.Virtual(adapter, 8, {adapter, renderer});
    }
  }
  ui.Call(0x82FE3F50, {manager + 0x284});
  if (const auto callback = Read(ui.base, manager + 0x248))
    ui.Call(callback, {3, 0, 0, Read(ui.base, manager + 0x24C)});
}
void FilterControllers(Native& ui) {
  const auto manager = Read(ui.base, preview.presentation + 0x20);
  for (const auto offset : {0x28u, 0x38u}) {
    const auto begin = Read(ui.base, manager + offset);
    const auto end = Read(ui.base, manager + offset + 4);
    if (!begin || end < begin || end - begin > 16384 || (end - begin) % 8) continue;
    auto output = begin;
    for (auto entry = begin; entry < end; entry += 8) {
      const auto wrapper = Read(ui.base, entry);
      const auto controller = Read(ui.base, entry + 4);
      // Borrowed dispatch lists only: retain native ownership of all objects.
      // Do not initialize MAP_OVERLAY, its camera/input hooks, or its details.
      if (!wrapper || !controller || Read(ui.base, controller) != 0x82075F24 ||
          !Descendant(ui.base, Read(ui.base, wrapper + 0x10), preview.group)) continue;
      Write(ui.base, output, wrapper);
      Write(ui.base, output + 4, controller);
      output += 8;
      preview.controller = controller;
    }
    Write(ui.base, manager + offset + 4, output);
  }
}
void DisableDispatch(Native& ui) {
  if (!preview.presentation) return;
  const auto manager = Read(ui.base, preview.presentation + 0x20);
  // Keep the native controller ownership map, but permanently empty both
  // borrowed dispatch lists after Init. Neither tick nor render may register
  // this donor's full-screen viewport or alter the existing HUD map record.
  for (const auto offset : {0x28u, 0x38u})
    Write(ui.base, manager + offset + 4, Read(ui.base, manager + offset));
}
void ReleaseHudResources(Native& ui) {
  requested_hud_record.store(0, std::memory_order_release);
  rendered_player_pixel.store(0, std::memory_order_release);
  if (!preview.hud_profile_ref) return;
  const auto profile = Read(ui.base, preview.hud_profile_ref);
  if (profile && preview.hud_resources_owned)
    ui.Call(0x82D937F8, {profile});
  ui.Call(0x824CEAB0, {preview.hud_profile_ref});
  REX_KERNEL_MEMORY()->SystemHeapFree(preview.hud_profile_ref);
  preview.hud_profile_ref = 0;
  preview.hud_resources_owned = false;
}

bool HudResources(Native& ui, uint32_t record) {
  const auto map = Read(ui.base, record + 0x20);
  const auto profile = Read(ui.base, map + 8);
  if (!profile) return false;
  if (preview.hud_profile_ref && Read(ui.base, preview.hud_profile_ref) != profile)
    ReleaseHudResources(ui);
  if (!preview.hud_profile_ref) {
    // Verify this is the named existing minimap profile before acquiring its
    // resources. The native callback supplies the original allocation context.
    Write(ui.base, ui.scratch + 640, 0);
    ui.Call(0x82D93940, {ui.scratch + 640, Read(ui.base, 0x8359DDE4), ui.String("minimap")});
    const bool matches = Read(ui.base, ui.scratch + 640) == profile;
    ui.Call(0x824CEAB0, {ui.scratch + 640});
    if (!matches) return false;
    preview.hud_profile_ref = REX_KERNEL_MEMORY()->SystemHeapAlloc(4);
    if (!preview.hud_profile_ref) return false;
    Write(ui.base, preview.hud_profile_ref, 0);
    ui.Call(0x82E55D58, {preview.hud_profile_ref, map + 8});
  }
  // Pause's first tick may precede the original HUD's leave callback. Its
  // later unload does not destroy the retained profile, but does clear its
  // resources, so acquire when that native transition has actually completed.
  if (!GuestPtr(ui.base, profile + 0x28)[0]) {
    ui.Call(0x8276F928);
    preview.hud_resources_owned = true;
    REXLOG_INFO("FH1 map preview: loaded existing minimap resources profile={:08X}", profile);
  }
  if (!ui.Call(0x82D93860, {profile})) return false;
  // Null textures are valid for untextured symbolizers, so the aggregate native
  // ready predicate alone cannot prove the two authored player icons are ready.
  const auto begin = Read(ui.base, map + 0x10), end = Read(ui.base, map + 0x14);
  if (!begin || end < begin || end - begin > 4096) return false;
  unsigned player_icons = 0;
  for (auto slot = begin; slot < end; slot += 4) {
    const auto layer = Read(ui.base, slot);
    const auto symbol = layer ? Read(ui.base, layer + 4) : 0;
    if (!symbol || Read(ui.base, symbol + 0xCC) != 8 || Read(ui.base, symbol + 0xD0) != 8 ||
        Read(ui.base, symbol + 0xC8) != 3 ||
        (Read(ui.base, symbol + 0xC4) != 2 && Read(ui.base, symbol + 0xC4) != 3)) continue;
    const auto resource = Read(ui.base, symbol + 4);
    const auto texture = resource ? Read(ui.base, resource + 12) : 0;
    if (!texture || GuestPtr(ui.base, texture + 0x28)[0] != 4 ||
        GuestPtr(ui.base, texture + 0x29)[0] != 4) return false;
    ++player_icons;
  }
  return player_icons >= 2;
}

uint32_t HudRecord(Native& ui) {
  const auto game = Read(ui.base, 0x833CAAE4);
  if (!game || !ui.Call(0x82F12410, {game})) return 0;
  const auto graphics = ui.Call(0x824ABD40, {game});
  if (!graphics || !Read(ui.base, graphics + 0x9A4)) return 0;
  const auto service = Read(ui.base, 0x833F0410);
  if (!service) return 0;
  const auto begin = Read(ui.base, service + 0x74);
  const auto end = Read(ui.base, service + 0x78);
  if (!begin || end < begin || end - begin > 256 || (end - begin) % 4) return 0;
  for (auto entry = begin; entry < end; entry += 4) {
    const auto record = Read(ui.base, entry);
    if (!record || Read(ui.base, record) != 0) continue;
    const auto map = Read(ui.base, record + 0x20);
    if (!map || !Read(ui.base, map + 4) || !Read(ui.base, record + 0x24) ||
        !GuestPtr(ui.base, record + 0x1C)[0])
      return 0;
    // Exact native CUIMapRenderer record0 pixel viewport. A record may exist
    // before the HUD has registered any drawable rectangle.
    const auto width = Read(ui.base, record + 12);
    const auto height = Read(ui.base, record + 16);
    return width && height && width <= 8192 && height <= 8192 ? record : 0;
  }
  return 0;
}
bool PlayerPixel(uint8_t* base, uint32_t record, float& px, float& py,
                 float& icon_size) {
  const auto map = Read(base, record + 0x20), camera = Read(base, record + 0x24);
  if (!map || !camera) return false;
  const auto begin = Read(base, map + 0x10), end = Read(base, map + 0x14);
  if (!begin || end < begin || end - begin > 4096) return false;
  for (auto slot = begin; slot < end; slot += 4) {
    const auto layer = Read(base, slot);
    const auto symbol = layer ? Read(base, layer + 4) : 0;
    if (!symbol || Read(base, symbol + 0xCC) != 8 || Read(base, symbol + 0xD0) != 8 ||
        Read(base, symbol + 0xC4) != 2 || Read(base, symbol + 0xC8) != 3) continue;
    const auto entries = Read(base, layer + 12), entries_end = Read(base, layer + 16);
    if (!entries || entries_end < entries || entries_end - entries > 4096) continue;
    for (auto entry = entries; entry < entries_end; entry += 64) {
      const auto entity = Read(base, entry + 0x14);
      if (!entity || Read(base, entity) != 0x82024360 ||
          !GuestPtr(base, entity + 0x14)[0]) continue;
      // Original icon builder82DA76A0 explicitly flattens Y at82DA77FC.
      // Project that same live player point through the existing HUD camera;
      // do not move its camera or invent a fixed marker at the crop center.
      const float x = Float(base, entity + 0x20), z = Float(base, entity + 0x28);
      const float w = x * Float(base, camera + 0x11C) +
                      z * Float(base, camera + 0x13C) + Float(base, camera + 0x14C);
      if (!std::isfinite(w) || w <= .001f) return false;
      const float nx = (x * Float(base, camera + 0x110) +
                       z * Float(base, camera + 0x130) + Float(base, camera + 0x140)) / w;
      const float ny = (x * Float(base, camera + 0x114) +
                       z * Float(base, camera + 0x134) + Float(base, camera + 0x144)) / w;
      if (!std::isfinite(nx) || !std::isfinite(ny)) return false;
      px = Read(base, record + 4) + (nx + 1.f) * .5f * Read(base, record + 12);
      py = Read(base, record + 8) + (1.f - ny) * .5f * Read(base, record + 16);
      icon_size = Float(base, symbol + 0x14);
      return std::isfinite(icon_size) && icon_size > 0.f;
    }
  }
  return false;
}

bool MirrorHud(Native& ui, uint32_t record) {
  if (!record || !preview.display_material) return false;
  const auto binding = ui.scratch + 576;
  const auto node = preview.display_material;
  Write(ui.base, binding, 0x820307A4);
  Write(ui.base, binding + 4, node);
  Write(ui.base, binding + 8, preview.presentation);
  const auto instance = node ? Read(ui.base, node + 4) : 0;
  const auto material = instance ? Read(ui.base, instance) : 0;
  const bool diagnose = preview.frames == 1 || preview.frames == 2 || preview.frames == 120;
  if (diagnose) REXLOG_INFO("FH1 map mirror binding: node={:08X} instance={:08X} material={:08X} record={:08X} rect={} {} {} {}",
      node, instance, material, record, Read(ui.base, record + 4), Read(ui.base, record + 8),
      Read(ui.base, record + 12), Read(ui.base, record + 16));
  if (!material) return false;
  const auto begin = Read(ui.base, material + 0xDC);
  const auto end = Read(ui.base, material + 0xE0);
  if (diagnose) REXLOG_INFO("FH1 map mirror images: begin={:08X} end={:08X}", begin, end);
  if (!begin || end < begin || end - begin > 64 || (end - begin) % 4 || begin == end)
    return false;
  if (diagnose) REXLOG_INFO("FH1 map mirror material: rgb={} {} {} opacity={} effective_alpha={}",
      Float(ui.base, material + 0x70), Float(ui.base, material + 0x74),
      Float(ui.base, material + 0x78), Float(ui.base, material + 0xB8), Float(ui.base, material + 0x7C));
  Write(ui.base, material + 0xB8, std::bit_cast<uint32_t>(1.f));
  const auto image = Read(ui.base, begin);
  if (!image) return false;
  const auto game = Read(ui.base, 0x833CAAE4);
  const auto graphics = game ? ui.Call(0x824ABD40, {game}) : 0;
  const auto target = graphics ? Read(ui.base, graphics + 0x9A4) : 0;
  if (!target) return false;
  if (Read(ui.base, image + 0xF4) != target) {
    // Exact original MapCtrl RT getter/ref-copy/material setter sequence.
    // The existing caption-paper image remains owned by the donor graph.
    ui.Call(0x8260A240, {ui.scratch + 560, graphics});
    ui.Call(0x82E55D58, {ui.scratch + 564, ui.scratch + 560});
    ui.Call(0x82F38478, {binding, ui.scratch + 564});
    ui.Call(0x824CEAB0, {ui.scratch + 560});
    ui.Color(node, 0xFFFFFF);
    // Color uses another scratch binding and leaves this one intact.
  }
  const auto texture = Read(ui.base, image + 0xF4);
  if (diagnose) REXLOG_INFO("FH1 map mirror texture: image={:08X} texture={:08X} type={}/{}",
      image, texture, texture ? GuestPtr(ui.base, texture + 0x28)[0] : 0,
      texture ? GuestPtr(ui.base, texture + 0x29)[0] : 0);
  if (!image || !texture) return false;
  // Same ready texture-size query used by CMapRendererCtrl::Init82892A60.
  const auto queried = ui.Call(0x82F384D8, {binding, ui.scratch + 544, ui.scratch + 548});
  if (diagnose) REXLOG_INFO("FH1 map mirror dimensions: queried={} width={} height={}",
      queried, Read(ui.base, ui.scratch + 544), Read(ui.base, ui.scratch + 548));
  if (!queried) return false;
  const auto tw = Read(ui.base, ui.scratch + 544);
  const auto th = Read(ui.base, ui.scratch + 548);
  const auto x = Read(ui.base, record + 4), y = Read(ui.base, record + 8);
  const auto width = Read(ui.base, record + 12), height = Read(ui.base, record + 16);
  if (!tw || !th || tw > 8192 || th > 8192 || x > tw || y > th ||
      width > tw - x || height > th - y) return false;
  // Fill without stretching. The HUD camera looks ahead of the player, so a
  // fixed center crop can clip its native arrow. Frame its projected position.
  constexpr float aspect = 360.f / 130.f;
  const float cw = std::min(float(width), float(height) * aspect);
  const float ch = std::min(float(height), float(width) / aspect);
  float player_x = float(x) + width * .5f, player_y = float(y) + height * .5f;
  float icon_size = 45.f;
  // The normal HUD tick briefly centers its camera, then native phase0 builds
  // the lead camera actually used to draw. Use that completed render snapshot.
  const auto pixel = rendered_player_pixel.load(std::memory_order_acquire);
  const bool projected = pixel != 0;
  if (projected) {
    player_x = std::bit_cast<float>(uint32_t(pixel >> 32));
    player_y = std::bit_cast<float>(uint32_t(pixel));
    icon_size = std::bit_cast<float>(rendered_player_size.load(std::memory_order_relaxed));
  }
  const float padding = 8.f * ch / 130.f;
  const float radius = icon_size * .5f;
  const float top = radius + padding;
  const float bottom = ch - radius - padding;
  const float target_y = top <= bottom ? std::clamp(ch * .5f, top, bottom) : ch * .5f;
  const float cx = std::clamp(player_x - cw * .5f, float(x), float(x + width) - cw);
  const float cy = std::clamp(player_y - target_y, float(y), float(y + height) - ch);
  const float su = cw / tw, sv = ch / th;
  const float pu = cx / cw, pv = (float(th) - cy - ch) / ch;
  // Generalize the exact native RT UV setup82892BB8..C50: image positions
  // are measured before scaling; V is bottom-origin. Build its cached matrix
  // through the original mapping-mode/rotation/pivot-aware function.
  Write(ui.base, image + 0xB4, std::bit_cast<uint32_t>(pu));
  Write(ui.base, image + 0xB8, std::bit_cast<uint32_t>(pv));
  Write(ui.base, image + 0xBC, std::bit_cast<uint32_t>(su));
  Write(ui.base, image + 0xC0, std::bit_cast<uint32_t>(sv));
  ui.ctx.f1.f64 = Float(ui.base, image + 0xB0);
  ui.ctx.f2.f64 = pu; ui.ctx.f3.f64 = pv;
  ui.ctx.f4.f64 = su; ui.ctx.f5.f64 = sv;
  ui.ctx.f6.f64 = Float(ui.base, image + 0xC4);
  ui.ctx.f7.f64 = Float(ui.base, image + 0xC8);
  ui.Call(0x82F1C600, {ui.scratch + 640, Read(ui.base, image + 0xCC)});
  std::copy_n(GuestPtr(ui.base, ui.scratch + 640), 64, GuestPtr(ui.base, image + 0x70));
  const std::array<uint32_t, 7> signature{record, x, y, width, height, tw, th};
  if (!preview.mirrored || signature != preview.mirror_signature || preview.frames == 120) {
    REXLOG_INFO("FH1 map preview: HUD mirror record={:08X} rect={} {} {} {} target={} {} crop={} {} {} {} image={:08X} player={} {} projected={} icon={}",
                record, x, y, width, height, tw, th, cx, cy, cw, ch, image,
                player_x, player_y, projected, icon_size);
    preview.mirror_signature = signature;
  }
  return true;
}
void Visibility(Native& ui, bool visible) {
  if (!preview.ready) return;
  ui.Call(0x82FEB480, {preview.presentation, uint32_t(!visible)});
  ui.Call(0x82FEB488, {preview.presentation, uint32_t(!visible)});
  GuestPtr(ui.base, preview.render_root + 0xFC)[0] = uint8_t(visible);
  GuestPtr(ui.base, preview.owner + 0x144)[0] = uint8_t(visible);
  const auto manager = ui.Virtual(preview.presentation, 0x54, {preview.presentation});
  const auto adapter = ui.Virtual(manager, 0x3C, {manager});
  if (adapter) GuestPtr(ui.base, adapter + 0x110)[0] = 1;
  preview.visible = visible;
}
void Start(Native& ui) {
  const auto preloader = ui.Call(0x824265C8, {0x54});
  if (!preloader) return;
  ui.Call(0x82F273B0, {preloader});
  Write(ui.base, preloader + 0x50, 0);
  constexpr char scene[] = "game:\\media\\ui\\scenes\\ui4\\FH1_PAUSE_MAP.bgf";
  ui.Call(0x8243D4D8, {preloader + 4, ui.String(scene), sizeof(scene) - 1});
  ui.Call(0x82CC2450, {preloader + 4});
  preview.preloader = preloader;
  ui.Call(0x82F30490, {preloader, 0x60000});
  REXLOG_INFO("FH1 map preview: requested native H_MAP alias");
}
bool Attach(Native& ui) {
  const auto reference = preview.preloader + 0x50;
  if (!Read(ui.base, reference)) ui.Call(0x82D0CAD0, {reference, preview.preloader + 0x20});
  const auto resource = Read(ui.base, reference);
  const auto presentation = resource ? Read(ui.base, resource + 0x74) : 0;
  if (!presentation) return false;
  const auto manager = ui.Virtual(presentation, 0x54, {presentation});
  Drain(ui, manager);
  const auto access = ui.Virtual(presentation, 0x1C, {presentation});
  const auto root = ui.Virtual(access, 0x48, {access});
  if (!root) return false;
  const auto group = ui.Find(access, root, "parent.Layer2.Map");
  const auto rectangle = ui.Find(access, root, "parent.Layer2.Map.Rectangle");
  if (!group || !rectangle) {
    if (preview.frames % 120 == 0)
      REXLOG_WARN("FH1 map preview: missing original Map/Rectangle root={:08X}", root);
    return false;
  }
  preview.presentation = presentation; preview.root = root;
  preview.group = group; preview.rectangle = rectangle;
  preview.caption = ui.Find(access, root, "parent.Layer.Tip.Bar.Text");
  preview.paper = ui.Find(access, root, "parent.Layer.Tip.Bar.Bar");
  preview.tip = ui.Find(access, root, "parent.Layer.Tip");
  preview.bar = ui.Find(access, root, "parent.Layer.Tip.Bar");
  // H_MAP's map rectangle is deliberately untextured (custom rendering).
  // Tip's native paper is already active and drawn with the caption. Reuse
  // its existing HUDPILL01 image instead of the slide-dependent map cursor.
  preview.display = preview.paper;
  preview.display_material = preview.paper
      ? ui.Find(access, preview.paper, "Material1504") : 0;
  if (!preview.display_material ||
      Read(ui.base, preview.display_material + 12) != preview.display ||
      !preview.caption || !preview.paper || !preview.tip || !preview.bar) return false;
  preview.display_ancestors.clear();
  for (auto node = preview.display; node && node != root; node = Read(ui.base, node + 12)) {
    if (Read(ui.base, node) == Hash("Layer")) break;
    preview.display_ancestors.push_back(node);
    if (preview.display_ancestors.size() > 32) return false;
  }
  // Keep the map and the donor's own text/paper caption, including cameras.
  // Tip's stacker is excluded from dispatch by FilterControllers.
  std::vector<uint32_t> ancestors;
  for (auto leaf : {group, preview.caption, preview.paper, preview.display}) {
    auto node = leaf;
    for (unsigned depth = 0; node && depth < 100; ++depth) {
      if (std::find(ancestors.begin(), ancestors.end(), node) == ancestors.end())
        ancestors.push_back(node);
      if (node == root) break;
      node = Read(ui.base, node + 12);
    }
    if (node != root) return false;
  }
  for (auto parent : ancestors) {
    if (parent == group || parent == preview.caption || parent == preview.paper ||
        parent == preview.display) continue;
    unsigned count = 0;
    for (auto child = Read(ui.base, parent + 20); child && count++ < 4096;
         child = Read(ui.base, child + 16)) {
      const auto name = Read(ui.base, child);
      const bool retain = name == Hash("Camera") || name == Hash("Light") ||
          std::find(ancestors.begin(), ancestors.end(), child) != ancestors.end();
      ui.Show(child, retain);
      if (!retain) preview.hidden.push_back(child);
    }
  }
  const auto runtime = ui.Virtual(presentation, 8, {presentation});
  const auto metadata = ui.Virtual(runtime, 0x1C, {runtime});
  const auto render_root = metadata ? Read(ui.base, metadata + 4) : 0;
  if (!runtime || !render_root) return false;
  const auto owner = ui.Call(0x824265C8, {0x148});
  if (!owner) return false;
  const auto root_name = Read(ui.base, preview.screen + 0x44) >= 16
      ? Read(ui.base, preview.screen + 0x30) : preview.screen + 0x30;
  ui.Call(0x82F2F398, {owner, presentation, ui.String("H_MAP"), root_name});
  Write(ui.base, owner + 0x10C, Read(ui.base, preview.screen + 0x10C));
  Write(ui.base, owner + 0x110, Read(ui.base, preview.screen + 0x110) + 1);
  Write(ui.base, owner + 0x11C, Read(ui.base, preview.screen + 0x11C));
  preview.owner = owner; preview.runtime = runtime; preview.render_root = render_root;
  Write(ui.base, metadata, owner);
  FilterControllers(ui);
  ui.Call(0x82F219E8, {owner});
  DisableDispatch(ui);
  GuestPtr(ui.base, owner + 0x4C)[0] = 1;
  const auto ui_manager = Read(ui.base, 0x833CAAC0);
  ui.Call(0x82700700, {owner + 0x5C, preview.preloader + 0x20});
  Write(ui.base, ui.scratch + 512, owner);
  ui.Call(0x82985560, {ui_manager + 0x24, Read(ui.base, ui_manager + 0x24), ui.scratch + 512});
  GuestPtr(ui.base, ui_manager + 0x5D)[0] = 1;
  const auto registry = Read(ui.base, ui_manager + 0x0C);
  if (registry) ui.Call(0x82F293E0, {registry, runtime});
  ui.Call(0x82F1EA48, {owner, 1});
  ui.Call(0x82F21950, {owner});
  ui.Call(0x82F1EC20, {owner, Hash("INTRO") & 0x7FFFFFFF});
  std::vector<uint32_t> pending{root};
  unsigned visited = 0;
  while (!pending.empty() && visited++ < 10000) {
    const auto node = pending.back(); pending.pop_back(); ui.Dirty(node);
    unsigned count = 0;
    for (auto child = Read(ui.base, node + 20); child && count++ < 4096;
         child = Read(ui.base, child + 16)) pending.push_back(child);
  }
  preview.ready = true;
  REXLOG_INFO("FH1 map preview: attached pres={:08X} controller={:08X} rectangle={:08X}",
              presentation, preview.controller, rectangle);
  return true;
}

bool Place(Native& ui) {
  if (preview.positioned) return true;
  if (!preview.controller) return false;
  // CUIMapRenderer::Register (826E2E20) uses this exact viewport interface.
  // Its rectangle is integer x,y,width,height in the current UI pixel space.
  const auto custom = Read(ui.base, preview.controller + 0x88);
  const auto viewport = custom ? ui.Virtual(custom + 4, 0x48, {custom + 4}) : 0;
  if (!viewport) return false;
  ui.Virtual(viewport, 0, {viewport, ui.scratch + 544});
  const auto x = int32_t(Read(ui.base, ui.scratch + 544));
  const auto y = int32_t(Read(ui.base, ui.scratch + 548));
  const auto width = Read(ui.base, ui.scratch + 552);
  const auto height = Read(ui.base, ui.scratch + 556);
  if (!width || !height || width > 8192 || height > 8192) return false;
  const auto instance = Read(ui.base, preview.rectangle + 4);
  if (!instance || Read(ui.base, instance + 4) != preview.rectangle) return false;
  const auto sx = Float(ui.base, instance + 0x24);
  const auto sy = Float(ui.base, instance + 0x28);
  if (!std::isfinite(sx) || !std::isfinite(sy) || sx == 0 || sy == 0) return false;
  // The donor uses the original 1280x720 orthographic UI camera. Its rectangle
  // is uniformly projected in UI pixels; retain the authored mesh and pivot.
  ui.Scalar(preview.rectangle, "scale.x", 0x24, sx * 360.f / width);
  ui.Scalar(preview.rectangle, "scale.y", 0x28, sy * 130.f / height);
  ui.Scalar(preview.rectangle, "position.x", 0xC,
            Float(ui.base, instance + 0xC) + 360.f - (x + width * .5f));
  ui.Scalar(preview.rectangle, "position.y", 0x10,
            Float(ui.base, instance + 0x10) - 287.f + (y + height * .5f));
  // H_MAP's original Tip/Bar ancestors are translation-only with unit scale.
  // Normalize those two groups rather than competing with their disabled stacker.
  // The original Bar mesh has authored [-50,50] bounds in both axes (FBF 0x45B).
  for (auto node : {preview.tip, preview.bar}) {
    ui.Scalar(node, "position.x", 0xC, 0.f);
    ui.Scalar(node, "position.y", 0x10, 0.f);
    // Both authored groups have pivot properties (Tip180/-60, Bar-176/50).
    for (const auto property : {"pivot.x", "pivot.y"}) {
      const auto hash = ui.Call(0x82F354E0, {ui.String(property)});
      ui.ctx.f1.f64 = 0.f;
      ui.Call(0x82F34DE0, {preview.presentation, node, hash});
    }
  }
  ui.Text(preview.caption, "WORLD MAP");
  ui.Scalar(preview.caption, "position.x", 0xC, -440.f);
  ui.Scalar(preview.caption, "position.y", 0x10, 155.f);
  ui.Scalar(preview.paper, "position.x", 0xC, -280.f);
  ui.Scalar(preview.paper, "position.y", 0x10, 73.f);
  ui.Scalar(preview.paper, "scale.x", 0x24, 3.6f);
  ui.Scalar(preview.paper, "scale.y", 0x28, 1.3f);
  preview.positioned = true;
  REXLOG_INFO("FH1 map preview: viewport {} {} {} {} -> 180 222 360 130 scale={} {}",
              x, y, width, height, sx, sy);
  return true;
}
void MaintainDisplay(Native& ui) {
  const bool diagnose = preview.frames == 2 || preview.frames == 120;
  if (diagnose) {
    for (auto node = preview.display; node; node = Read(ui.base, node + 12)) {
      const auto instance = Read(ui.base, node + 4);
      if (instance && Read(ui.base, instance + 4) == node)
        REXLOG_INFO("FH1 map quad: node={:08X} name={:08X} type={} local={} {} {} world={} {} {} scale={} {} alpha={} flags={:02X}/{:02X}",
            node, Read(ui.base, node), GuestPtr(ui.base, instance + 0xA)[0],
            Float(ui.base, instance + 0xC), Float(ui.base, instance + 0x10), Float(ui.base, instance + 0x14),
            Float(ui.base, instance + 0xAC), Float(ui.base, instance + 0xB0), Float(ui.base, instance + 0xB4),
            Float(ui.base, instance + 0x24), Float(ui.base, instance + 0x28), Float(ui.base, instance + 0xE0),
            GuestPtr(ui.base, instance + 0xFB)[0], GuestPtr(ui.base, instance + 0xFF)[0]);
      if (node == preview.root) break;
    }
  }
  // Tip.Bar.Bar references the original 100x100 paper quad. Its caption
  // remains a sibling above the image; reserve the card's top42px so roads
  // cannot reduce its contrast. The Tip stacker never enters dispatch.
  for (auto node : preview.display_ancestors) {
    ui.Opacity(node);
    ui.Scalar(node, "position.x", 0xC, node == preview.display ? -280.f : 0.f);
    ui.Scalar(node, "position.y", 0x10, node == preview.display ? 73.f : 0.f);
    ui.Scalar(node, "position.z", 0x14, 0.f);
    ui.Scalar(node, "scale.x", 0x24, node == preview.display ? 3.6f : 1.f);
    ui.Scalar(node, "scale.y", 0x28, node == preview.display ? 1.3f : 1.f);
    for (const auto property : {"pivot.x", "pivot.y", "rotation.z"}) {
      const auto hash = ui.Call(0x82F354E0, {ui.String(property)});
      ui.ctx.f1.f64 = 0.f;
      ui.Call(0x82F34DE0, {preview.presentation, node, hash});
    }
  }
  ui.CaptionFont();
}
} // namespace

bool Ready() { return preview.ready && preview.positioned && preview.mirrored && preview.visible; }
void Update(const PPCContext& context, uint8_t* base, uint32_t screen, bool visible,
            bool /*selected*/) {
  if (!base || !screen) return;
  if (preview.screen && preview.screen != screen) Release(context, base);
  Native ui(context, base);
  if (!ui.scratch) return;
  preview.screen = screen;
  if (!visible) {
    ReleaseHudResources(ui);
    Visibility(ui, false);
    return;
  }
  const auto record = HudRecord(ui);
  if (!record) { ReleaseHudResources(ui); preview.mirrored = false; Visibility(ui, false); return; }
  if (!HudResources(ui, record)) {
    requested_hud_record.store(0, std::memory_order_release);
    rendered_player_pixel.store(0, std::memory_order_release);
    preview.mirrored = false;
    Visibility(ui, false);
    return;
  }
  if (!preview.preloader) Start(ui);
  if (!preview.preloader) return;
  ++preview.frames;
  if (!preview.ready && !Attach(ui)) return;
  for (auto node : preview.hidden) ui.Show(node, false);
  for (auto leaf : {preview.group, preview.caption, preview.paper, preview.display})
    for (auto node = leaf; node; node = Read(base, node + 12)) {
      ui.Show(node, true);
      if (node == preview.root) break;
    }
  ui.Call(0x82FEE208, {preview.presentation});
  const auto manager = Read(base, preview.presentation + 0x14);
  const auto clock = ui.Virtual(manager + 8, 0x10, {manager + 8});
  ui.Call(0x82FE39A0, {preview.runtime});
  ui.Call(0x82FECE38, {preview.presentation, clock});
  const bool placed = Place(ui);
  if (placed) MaintainDisplay(ui);
  // No custom-render placeholder is drawn: only the existing textured quad.
  ui.Show(preview.rectangle, false);
  preview.mirrored = placed && MirrorHud(ui, record);
  // Publish only a native-owned record ID. The render hook validates membership
  // again and prepares its transient icon buffers at the actual material pass.
  requested_hud_record.store(preview.mirrored ? record : 0, std::memory_order_release);
  Visibility(ui, preview.mirrored);
}
void Release(const PPCContext& context, uint8_t* base) {
  requested_hud_record.store(0, std::memory_order_release);
  if (!base || !preview.screen) return;
  Native ui(context, base);
  if (!ui.scratch) return;
  ReleaseHudResources(ui);
  Visibility(ui, false);
  if (preview.owner)
    ui.Call(0x82F2F828, {Read(base, 0x833CAAC0), preview.owner, 0});
  if (preview.preloader) {
    ui.Call(0x82EB2C98, {preview.preloader + 0x50});
    ui.Call(0x824CEAB0, {preview.preloader + 0x50});
    ui.Call(0x82F27400, {preview.preloader});
    ui.Call(0x8242D1A0, {preview.preloader});
  }
  preview = {};
}
// Prepare at the original phase0 boundary so transient dynamic-layer buffers
// are rebuilt immediately before their native render pass consumes them.
struct HudPassScope {
  uint8_t* base = nullptr;
  uint32_t camera = 0, fov = 0;
  std::array<uint8_t, 256> matrices{};
  static constexpr float span = 3.f;

  bool Apply(uint8_t* memory, uint32_t camera_address) {
    const float original_fov = Float(memory, camera_address + 0x34);
    const float original_distance = Float(memory, camera_address + 0x38);
    if (!std::isfinite(original_fov) || original_fov <= 0.f || original_fov >= 3.0f ||
        !std::isfinite(original_distance) || original_distance <= 0.f) return false;
    const float preview_fov = 2.f * std::atan(span * std::tan(original_fov * .5f));
    if (!std::isfinite(preview_fov) || preview_fov >= 3.13f) return false;
    base = memory;
    camera = camera_address;
    fov = Read(base, camera + 0x34);
    std::copy_n(GuestPtr(base, camera + 0x50), matrices.size(), matrices.begin());
    // Native82DA2648 builds projection cot(FOV/2). Increasing tan(FOV/2)
    // threefold expands the visible world span while preserving camera distance
    // and every native icon's DynamicSize, offsets and distance-dependent state.
    // DynamicSize scales world geometry by distance (82DA7A58), not FOV, so
    // original45px player icons become15px in the source /21px in this card.
    Write(base, camera + 0x34, std::bit_cast<uint32_t>(preview_fov));
    static thread_local uint32_t logged_camera = 0;
    if (logged_camera != camera) {
      REXLOG_INFO("FH1 map preview: scoped FOV camera={:08X} distance={} fov={} -> {} pitch={} unchanged span=3 native_icons_unchanged",
          camera, original_distance, original_fov, preview_fov, Float(base, camera + 0x30));
      logged_camera = camera;
    }
    return true;
  }

  ~HudPassScope() {
    if (!camera) return;
    // Native draw constants are copied during the pass; restore shared state.
    Write(base, camera + 0x34, fov);
    std::copy(matrices.begin(), matrices.end(), GuestPtr(base, camera + 0x50));
  }
};

void PrepareHudPass(const PPCContext& context, uint8_t* base, HudPassScope& scope) {
  const auto record = requested_hud_record.load(std::memory_order_acquire);
  if (!record || context.r4.u32 != 0 || !base) return;
  const auto service = context.r3.u32;
  if (!service || service != Read(base, 0x833F0410)) return;
  const auto begin = Read(base, service + 0x74);
  const auto end = Read(base, service + 0x78);
  if (!begin || end < begin || end - begin > 256 || (end - begin) % 4) return;
  bool found = false;
  for (auto entry = begin; entry < end; entry += 4)
    if (Read(base, entry) == record) { found = true; break; }
  if (!found || Read(base, record) != 0 || !GuestPtr(base, record + 0x1C)[0]) return;
  const auto map = Read(base, record + 0x20);
  const auto camera = Read(base, record + 0x24);
  if (!map || !Read(base, map + 4) || !camera || !Read(base, record + 12) ||
      !Read(base, record + 16)) return;
  if (!scope.Apply(base, camera)) return;
  // Exact Register826E2E20 preparation tail with temporary preview framing.
  // Do not register the donor or change the existing record's viewport.
  PPCContext call = context;
  call.r3.u64 = camera;
  rex::runtime::ResolveIndirectFunction(0x82DA2388)(call, base);
  call = context; call.r3.u64 = map; call.r4.u64 = camera;
  rex::runtime::ResolveIndirectFunction(0x82DA4A80)(call, base);
  float player_x = 0.f, player_y = 0.f, icon_size = 0.f;
  if (PlayerPixel(base, record, player_x, player_y, icon_size)) {
    icon_size /= HudPassScope::span;
    rendered_player_size.store(std::bit_cast<uint32_t>(icon_size), std::memory_order_relaxed);
    rendered_player_pixel.store((uint64_t(std::bit_cast<uint32_t>(player_x)) << 32) |
                               std::bit_cast<uint32_t>(player_y), std::memory_order_release);
  }
  GuestPtr(base, record + 0x1D)[0] = 1;
  static thread_local uint32_t diagnostic_record = 0;
  static thread_local unsigned diagnostic_frames = 0;
  if (diagnostic_record != record) { diagnostic_record = record; diagnostic_frames = 0; }
  if (++diagnostic_frames == 2 || diagnostic_frames == 120) {
    const auto layers_begin = Read(base, map + 0x10);
    const auto layers_end = Read(base, map + 0x14);
    if (layers_begin && layers_end >= layers_begin && layers_end - layers_begin <= 4096) {
      for (auto slot = layers_begin; slot < layers_end; slot += 4) {
        const auto layer = Read(base, slot);
        const auto profile = layer ? Read(base, layer + 4) : 0;
        if (!profile || Read(base, profile + 0xCC) != 8 || Read(base, profile + 0xD0) != 8 ||
            Read(base, profile + 0xC8) != 3 ||
            (Read(base, profile + 0xC4) != 2 && Read(base, profile + 0xC4) != 3)) continue;
        const auto resource = Read(base, profile + 4);
        const auto texture = resource ? Read(base, resource + 0xC) : 0;
        const auto entries = Read(base, layer + 0xC), entries_end = Read(base, layer + 0x10);
        REXLOG_INFO("FH1 map player layer: layer={:08X} profile={:08X} atlas={} {} enabled={} dynamic={} size={} texture={:08X} type={}/{} entries={:08X}..{:08X}",
            layer, profile, Read(base, profile + 0xC4), Read(base, profile + 0xC8),
            GuestPtr(base, layer + 0x20)[0], GuestPtr(base, profile + 0x90)[0], Float(base, profile + 0x14),
            texture, texture ? GuestPtr(base, texture + 0x28)[0] : 0,
            texture ? GuestPtr(base, texture + 0x29)[0] : 0, entries, entries_end);
        if (!entries || entries_end < entries || entries_end - entries > 4096) continue;
        for (auto entry = entries; entry < entries_end; entry += 64) {
          const auto entity = Read(base, entry + 0x14), geometry = Read(base, entry + 0x18);
          REXLOG_INFO("FH1 map player entry: entry={:08X} entity={:08X} vtable={:08X} enabled={} xyz={} {} {} visible={} geometry={:08X} data={:08X}/{:08X}/{:08X}",
              entry, entity, entity ? Read(base, entity) : 0,
              entity ? GuestPtr(base, entity + 0x14)[0] : 0,
              entity ? Float(base, entity + 0x20) : 0.f, entity ? Float(base, entity + 0x24) : 0.f,
              entity ? Float(base, entity + 0x28) : 0.f, GuestPtr(base, entry + 0x2C)[0], geometry,
              geometry ? Read(base, geometry) : 0, geometry ? Read(base, geometry + 4) : 0,
              geometry ? Read(base, geometry + 8) : 0);
        }
      }
    }
  }
  static thread_local uint32_t logged_record = 0;
  if (logged_record != record) {
    REXLOG_INFO("FH1 map preview: prepared HUD geometry at native phase0 record={:08X}", record);
    logged_record = record;
  }
}
} // namespace forza::fh1_map_preview
extern "C" REX_FUNC(sub_826E22A8) {
  forza::fh1_map_preview::HudPassScope scope;
  forza::fh1_map_preview::PrepareHudPass(ctx, base, scope);
  __imp__sub_826E22A8(ctx, base);
}
#endif
