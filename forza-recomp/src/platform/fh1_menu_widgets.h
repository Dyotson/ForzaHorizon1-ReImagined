#pragma once
#include "guest_addr.h"

#if FORZA_TU4
#include "guest_memory.h"
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <algorithm>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

// FH1 TU4 presentation bindings. No visual nodes are manufactured here: every
// handle refers to a node owned by a separately loaded FH1 authored scene.
namespace forza::fh1_menu_widgets {
inline uint32_t Read(uint8_t* base, uint32_t address) {
  const auto* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void Write(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = GuestPtr(base, address);
  for (int i = 3; i >= 0; --i) { p[i] = uint8_t(value); value >>= 8; }
}
inline float Float(uint8_t* base, uint32_t address) { return std::bit_cast<float>(Read(base, address)); }
inline uint32_t Hash(std::string_view text) {
  uint32_t result = 0;
  for (unsigned char c : text) result = c + result * 65599;
  return result * 65599; // Native SDBM includes the terminating zero.
}
// Invalid UTF-8 bytes become U+FFFD. Capacity counts UTF-16 code units,
// including both surrogate units for supplementary characters.
inline bool DecodeUtf8(std::string_view input, std::u16string& output, size_t capacity=240) {
  output.clear();
  for (size_t i=0; i<input.size();) {
    const auto lead=uint8_t(input[i]);
    uint32_t point=0xFFFD;
    size_t count=1;
    unsigned expected=0;
    if (lead<0x80) point=lead;
    else if (lead>=0xC2 && lead<=0xDF) { point=lead&0x1F; expected=2; }
    else if (lead>=0xE0 && lead<=0xEF) { point=lead&0x0F; expected=3; }
    else if (lead>=0xF0 && lead<=0xF4) { point=lead&7; expected=4; }
    if (expected) {
      bool valid=expected<=input.size()-i;
      if (valid) for (unsigned j=1; j<expected; ++j) {
        const auto byte=uint8_t(input[i+j]);
        if ((byte&0xC0)!=0x80) { valid=false; break; }
        point=(point<<6)|(byte&0x3F);
      }
      const uint32_t minimum=expected==2 ? 0x80u : expected==3 ? 0x800u : 0x10000u;
      if (!valid || point<minimum || point>0x10FFFF || (point>=0xD800 && point<=0xDFFF)) point=0xFFFD;
      else count=expected;
    }
    const size_t units=point>0xFFFF ? 2 : 1;
    if (output.size()>capacity || units>capacity-output.size()) { output.clear(); return false; }
    if (point>0xFFFF) {
      point-=0x10000;
      output.push_back(char16_t(0xD800+(point>>10)));
      output.push_back(char16_t(0xDC00+(point&0x3FF)));
    } else output.push_back(char16_t(point));
    i+=count;
  }
  return true;
}
inline bool IsDescendant(uint8_t* base, uint32_t node, uint32_t ancestor) {
  if (!ancestor) return false;
  for (unsigned depth = 0; node && depth < 128; ++depth) {
    if (node == ancestor) return true;
    node = Read(base, node + 12);
  }
  return false;
}
inline std::vector<uint32_t> Descendants(uint8_t* base, uint32_t root, uint32_t name) {
  std::vector<uint32_t> found, pending;
  if (root) pending.push_back(root);
  unsigned visited = 0;
  while (!pending.empty() && visited++ < 8192) {
    const auto node = pending.back(); pending.pop_back();
    if (Read(base, node) == name) found.push_back(node);
    unsigned siblings = 0;
    for (auto child = Read(base, node + 20); child && siblings++ < 1024;
         child = Read(base, child + 16)) pending.push_back(child);
    if (pending.size() > 8192) break;
  }
  return found;
}

class Native {
public:
  PPCContext ctx;
  uint8_t* base;
  uint32_t scratch;
  Native(const PPCContext& context, uint8_t* memory)
      : ctx(context), base(memory), scratch(memory ? REX_KERNEL_MEMORY()->SystemHeapAlloc(2048) : 0) {}
  ~Native() { if (scratch) REX_KERNEL_MEMORY()->SystemHeapFree(scratch); }
  Native(const Native&) = delete;
  Native& operator=(const Native&) = delete;
  explicit operator bool() const { return base && scratch; }
  uint32_t Call(uint32_t address, std::initializer_list<uint32_t> args = {}) {
    if (!address || !base || args.size() > 7) return 0;
    unsigned i = 0;
    for (auto value : args) switch (i++) {
      case 0: ctx.r3.u64=value; break; case 1: ctx.r4.u64=value; break;
      case 2: ctx.r5.u64=value; break; case 3: ctx.r6.u64=value; break;
      case 4: ctx.r7.u64=value; break; case 5: ctx.r8.u64=value; break;
      case 6: ctx.r9.u64=value; break;
    }
    rex::runtime::ResolveIndirectFunction(address)(ctx, base);
    return ctx.r3.u32;
  }
  uint32_t Virtual(uint32_t object, uint32_t slot, std::initializer_list<uint32_t> args) {
    return object && Read(base, object) ? Call(Read(base, Read(base, object) + slot), args) : 0;
  }
  uint32_t String(std::string_view text) {
    if (!scratch || text.size() > 511) return 0;
    std::copy(text.begin(), text.end(), GuestPtr(base, scratch + 1024));
    GuestPtr(base, scratch + 1024)[text.size()] = 0;
    return scratch + 1024;
  }
  uint32_t Find(uint32_t access, uint32_t root, std::string_view path) {
    const auto text = String(path);
    return access && root && text ? Call(0x82F348B8, {access, root, text, 0}) : 0;
  }
  uint32_t PropertyHash(std::string_view name) {
    const auto text = String(name);
    return text ? Call(0x82F354E0, {text}) : 0;
  }
  uint32_t Binding(uint32_t presentation, uint32_t node) {
    if (!scratch || !presentation || !node) return 0;
    Write(base, scratch + 768, 0x820307A4);
    Write(base, scratch + 772, node); Write(base, scratch + 776, presentation);
    return scratch + 768;
  }
  uint32_t TextRef(std::string_view text) {
    std::u16string decoded;
    if (!scratch || !DecodeUtf8(text,decoded,240)) return 0;
    auto* p = GuestPtr(base, scratch + 32);
    for (size_t i=0; i<decoded.size(); ++i) {
      p[2*i]=uint8_t(decoded[i]>>8); p[2*i+1]=uint8_t(decoded[i]);
    }
    p[2*decoded.size()]=p[2*decoded.size()+1]=0;
    const auto object = Call(0x824265C8, {16});
    if (!object) return 0;
    Write(base, object, 0x82001674);
    for (unsigned i = 4; i < 16; i += 4) Write(base, object + i, 0);
    Call(0x82D6DE20, {object, scratch + 32, uint32_t(decoded.size())});
    Call(0x82555B90, {scratch, object});
    return scratch;
  }
  // Caller must resolve an authored TEXT node, never a group or material.
  bool Text(uint32_t presentation, uint32_t node, std::string_view text) {
    if (!node || !presentation || !TextRef(text)) return false;
    Call(0x82F3A728, {Binding(presentation, node), scratch});
    Call(0x824CEAB0, {scratch}); return true;
  }
  // Mirror native localized text without adopting its controller's layout.
  // 82F3A490 owns the UTF-16 string at text-object+84 (8-unit small buffer).
  bool CopyText(uint32_t presentation,uint32_t source,uint32_t target,uint64_t& last) {
    const auto wrapper=source?Read(base,source+4):0;
    if(!wrapper||Read(base,wrapper+4)!=source||GuestPtr(base,wrapper+0xA)[0]!=5)return false;
    const auto object=Read(base,wrapper);if(!object)return false;
    const auto length=Read(base,object+0x94),capacity=Read(base,object+0x98);
    if(!length||length>240||length>capacity)return false;
    const auto chars=capacity<8?object+0x84:Read(base,object+0x84);if(!chars)return false;
    uint64_t hash=14695981039346656037ull;
    for(unsigned i=0;i<length*2;++i){hash^=GuestPtr(base,chars+i)[0];hash*=1099511628211ull;}
    if(hash!=last){Call(0x82F3A490,{Binding(presentation,target),chars});last=hash;}
    return true;
  }
  bool Scalar(uint32_t presentation, uint32_t node, std::string_view name, float value) {
    if (!node || !presentation || !std::isfinite(value)) return false;
    const auto hash = PropertyHash(name);
    if (!Call(0x82FEF130, {node, hash})) return false;
    ctx.f1.f64 = value; Call(0x82F34DE0, {presentation, node, hash}); return true;
  }
  void Dirty(uint32_t presentation, uint32_t node) {
    if (!presentation || !node || !scratch) return;
    const auto queue = Virtual(presentation, 0x7C, {presentation});
    Write(base, scratch + 800, node);
    if (queue) Call(0x82FE57F0, {queue, scratch + 800});
  }
  void Show(uint32_t presentation, uint32_t node, bool visible) {
    if (const auto binding = Binding(presentation, node)) {
      Call(0x82F35B48, {binding, uint32_t(visible)}); Dirty(presentation, node);
    }
  }
  bool Transform(uint32_t presentation, uint32_t node, float x, float y, bool scale) {
    if (!node || !std::isfinite(x) || !std::isfinite(y)) return false;
    const auto instance = Read(base, node + 4);
    if (!instance || Read(base, instance + 4) != node) return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
      const char* name = scale ? (axis ? "scale.y" : "scale.x") : (axis ? "position.y" : "position.x");
      const float value = axis ? y : x;
      if (!Scalar(presentation, node, name, value)) {
        Write(base, instance + (scale ? 0x24 : 0xC) + axis*4, std::bit_cast<uint32_t>(value));
        GuestPtr(base, instance + 0xFB)[0] |= 0x20; Dirty(presentation, node);
      }
    }
    return true;
  }
  bool Position(uint32_t p, uint32_t n, float x, float y) { return Transform(p,n,x,y,false); }
  bool Scale(uint32_t p, uint32_t n, float x, float y) { return Transform(p,n,x,y,true); }
  bool RotationZ(uint32_t presentation, uint32_t node, float angle) {
    if (!presentation || !node || !std::isfinite(angle)) return false;
    const auto instance=Read(base,node+4);
    if (!instance || Read(base,instance+4)!=node) return false;
    if (!Scalar(presentation,node,"rotation.z",angle)) {
      // FH1 82FE3BD8: the rotation.z property hash writes instance+20,
      // then 82FE3C60 sets the transform-dirty bit. Static nodes lack
      // that property, but use the same native transform cache.
      Write(base,instance+0x20,std::bit_cast<uint32_t>(angle));
      GuestPtr(base,instance+0xFB)[0] |= 0x20;
      Dirty(presentation,node);
    }
    return true;
  }
  // FH1 82FE6238 builds the node-local matrix at +3C before combining it
  // with its ancestors. Supply a 2D shear there; leave the label nodes alone.
  bool TabShear(uint32_t presentation,uint32_t node,float width,float height) {
    const auto instance=node?Read(base,node+4):0;
    if(!instance||Read(base,instance+4)!=node||!std::isfinite(width)||
        !std::isfinite(height)||width<=0||height<=0)return false;
    for(unsigned axis=0;axis<3;++axis)Write(base,instance+0x18+axis*4,0);
    Scale(presentation,node,width/100,height/100);
    Call(0x82FE4368,{instance+0x3C,instance+0xC,instance+0x18,
        instance+0x24,instance+0x30,GuestPtr(base,instance+0xF9)[0],GuestPtr(base,instance+0xF8)[0]});
    Write(base,instance+0x3C+0x10,std::bit_cast<uint32_t>(.28f*height/100));
    // Keep our local matrix; request only the native world-matrix update.
    GuestPtr(base,instance+0xFB)[0]=(GuestPtr(base,instance+0xFB)[0]&~0x20)|0x10;
    Dirty(presentation,node);return true;
  }
  void Opacity(uint32_t presentation, uint32_t node, float percent) {
    if (!node || !std::isfinite(percent) || Scalar(presentation,node,"opacity",percent)) return;
    const auto instance = Read(base,node+4);
    if (instance && Read(base,instance+4)==node) {
      Write(base,instance+0xE0,std::bit_cast<uint32_t>(std::clamp(percent/100.f,0.f,1.f)));
      GuestPtr(base,instance+0xFB)[0] |= 0x80; Dirty(presentation,node);
    }
  }
  void Color(uint32_t presentation, uint32_t node, uint32_t rgb, bool text=false) {
    if (const auto binding = Binding(presentation,node)) {
      if (!text) {
        const auto wrapper=Read(base,node+4), material=wrapper ? Read(base,wrapper) : 0;
        if (material) for (unsigned channel=0; channel<3; ++channel) {
          const float current=Float(base,material+0x70+channel*4);
          if (!std::isfinite(current) || current<0.f) continue;
          const float value=float((rgb>>(16-channel*8))&255)/255.f;
          Write(base,material+0x70+channel*4,std::bit_cast<uint32_t>(value));
          Write(base,material+0xA0+channel*4,std::bit_cast<uint32_t>(value));
        }
      }
      Call(text ? 0x82F37BE0 : 0x82F37C40,{binding,rgb});
    }
  }
  bool CropImage(uint32_t material_node,float u,float v,float width,float height) {
    const auto wrapper=material_node?Read(base,material_node+4):0;
    const auto material=wrapper?Read(base,wrapper):0;
    if(!material||width<=0||height<=0)return false;
    const auto begin=Read(base,material+0xDC),end=Read(base,material+0xE0);
    if(!begin||end<=begin||end-begin>64||(end-begin)%4)return false;
    const auto image=Read(base,begin);if(!image)return false;
    const float pu=u/width,pv=(1-v-height)/height;
    Write(base,image+0xB4,std::bit_cast<uint32_t>(pu));Write(base,image+0xB8,std::bit_cast<uint32_t>(pv));
    Write(base,image+0xBC,std::bit_cast<uint32_t>(width));Write(base,image+0xC0,std::bit_cast<uint32_t>(height));
    ctx.f1.f64=Float(base,image+0xB0);ctx.f2.f64=pu;ctx.f3.f64=pv;
    ctx.f4.f64=width;ctx.f5.f64=height;ctx.f6.f64=Float(base,image+0xC4);ctx.f7.f64=Float(base,image+0xC8);
    Call(0x82F1C600,{scratch+640,Read(base,image+0xCC)});
    std::copy_n(GuestPtr(base,scratch+640),64,GuestPtr(base,image+0x70));return true;
  }
  // Width/height are native normalized text-box units, not screen pixels.
  // The caller must use a text node and an access interface for this presentation.
  bool TextStyle(uint32_t access, uint32_t presentation, uint32_t node,
                 float size, float width, float height, uint32_t alignment) {
    if (!access || !node || !std::isfinite(size) || !std::isfinite(width) ||
        !std::isfinite(height) || size<1.f || size>4096.f || width<=0 || height<=0 || alignment>2) return false;
    bool complete=Scalar(presentation,node,"size",size);
    complete=Scalar(presentation,node,"boxwidth",width) && complete;
    complete=Scalar(presentation,node,"boxheight",height) && complete;
    const auto integer=[&](const char* name,uint32_t value) {
      const auto property=Call(0x82FEF130,{node,PropertyHash(name)});
      if (!property || (Read(base,property)&0x1C)!=4) return false;
      Call(0x82FF2748,{access,node,property,value}); return true;
    };
    complete=integer("horzalign",alignment) && complete;
    complete=integer("texttype",1) && complete;
    // wordwrap is a native boolean property, a different tagged type.
    const auto wrap=Call(0x82FEF130,{node,PropertyHash("wordwrap")});
    if (wrap && (Read(base,wrap)&0x1C)==8) Call(0x82FF2748,{access,node,wrap,0});
    const auto instance=Read(base,node+4), text=instance ? Read(base,instance) : 0;
    if (text && Read(base,instance+4)==node) {
      // Constant FH1 text nodes can omit authored properties. Match the
      // native82F2A8D0/82F2AC00 caches and invalidate fitting/geometry.
      Write(base,text+0x70,std::bit_cast<uint32_t>(height));
      Write(base,text+0x74,std::bit_cast<uint32_t>(width));
      Write(base,text+0x80,uint32_t(size));
      Write(base,text+0x7C,1); Write(base,text+0xDC,2);
      Write(base,text+0xC0,alignment); Write(base,text+0xC4,0);
      GuestPtr(base,text+0x104)[0]=0;
      GuestPtr(base,text+0x105)[0]=1;
      GuestPtr(base,text+0x10C)[0]=1;
      complete=true;
    }
    return complete;
  }
};

struct FooterButton { uint32_t glyph; std::string_view label; };
// The initialized native footer owns its atlas/style mapping and row spacing.
// Glyph IDs are native model IDs, not texture coordinates.
inline bool FooterButtons(Native& ui, uint32_t screen,
                          std::initializer_list<FooterButton> buttons) {
  const auto footer=ui.Call(0x82746638,{screen});
  if (!footer) return false;
  ui.Call(0x8289A908,{footer});
  for (const auto& button:buttons) if (!button.label.empty()) {
    const auto text=ui.TextRef(button.label);
    if (!text) return false;
    ui.Call(0x8289E2B0,{footer,button.glyph,text});
    ui.Call(0x824CEAB0,{text});
  }
  ui.Call(Read(ui.base,Read(ui.base,footer+0x6C)+0x28),{footer+0x6C});
  return true;
}
struct Donor {
  enum class Status { Idle, Loading, Ready, Failed };
  using KeepController = std::function<bool(uint32_t wrapper, uint32_t controller)>;
  Status status = Status::Idle;
  uint32_t preloader=0, presentation=0, access=0, root=0, runtime=0, owner=0, render_root=0;
  bool visible=false;
  std::vector<uint32_t> retained_ancestry, hidden;
  // Explicit Release on the guest UI thread is mandatory; host destruction
  // cannot safely call guest functions without the live PPC context.
  Donor() = default;
  Donor(const Donor&) = delete;
  Donor& operator=(const Donor&) = delete;
  Donor(Donor&& other) noexcept { Take(other); }
  Donor& operator=(Donor&& other) noexcept {
    if (this!=&other) {
      // Release requires the guest UI context; never overwrite a live owner.
      assert(!preloader && "Release donor before move assignment");
      if (!preloader) Take(other);
    }
    return *this;
  }
  bool Ready() const { return status == Status::Ready; }
  bool Start(Native& ui, std::string_view alias_path) {
    if (!ui || status != Status::Idle || alias_path.empty() || alias_path.size()>240) return false;
    preloader=ui.Call(0x824265C8,{0x54});
    if (!preloader) { status=Status::Failed; return false; }
    ui.Call(0x82F273B0,{preloader}); Write(ui.base,preloader+0x50,0);
    ui.Call(0x8243D4D8,{preloader+4,ui.String(alias_path),uint32_t(alias_path.size())});
    ui.Call(0x82CC2450,{preloader+4}); ui.Call(0x82F30490,{preloader,0x60000});
    status=Status::Loading; return true;
  }
  bool Poll(Native& ui, uint32_t parent_screen, const KeepController& keep = {}, int32_t layer_offset = 1) {
    if (Ready()) return true;
    if (!ui || status != Status::Loading || !parent_screen) return false;
    const auto ref=preloader+0x50;
    if (!Read(ui.base,ref)) ui.Call(0x82D0CAD0,{ref,preloader+0x20});
    const auto resource=Read(ui.base,ref);
    presentation=resource ? Read(ui.base,resource+0x74) : 0;
    if (!presentation) return false;
    const auto manager=ui.Virtual(presentation,0x54,{presentation});
    if (!Drain(ui,manager)) { status=Status::Failed; return false; }
    access=ui.Virtual(presentation,0x1C,{presentation});
    root=ui.Virtual(access,0x48,{access});
    runtime=ui.Virtual(presentation,8,{presentation});
    const auto metadata=ui.Virtual(runtime,0x1C,{runtime});
    render_root=metadata ? Read(ui.base,metadata+4) : 0;
    if (!root || !runtime || !render_root) return false;
    // Both borrowed dispatch arrays are validated before either is modified.
    // The ownership map remains untouched, including rejected controllers.
    if (!Filter(ui,keep)) { status=Status::Failed; return false; }
    const auto ui_manager=Read(ui.base,0x833CAAC0);
    if (!ui_manager || !Read(ui.base,ui_manager+0x0C)) return false;
    owner=ui.Call(0x824265C8,{0x148});
    if (!owner) { status=Status::Failed; return false; }
    const auto root_name=Read(ui.base,parent_screen+0x44)>=16 ? Read(ui.base,parent_screen+0x30) : parent_screen+0x30;
    ui.Call(0x82F2F398,{owner,presentation,ui.String("108_C_BUY_CARSELECT"),root_name});
    Write(ui.base,owner+0x10C,Read(ui.base,parent_screen+0x10C));
    const auto parent_layer=Read(ui.base,parent_screen+0x110);
    Write(ui.base,owner+0x110,uint32_t(int64_t(parent_layer)+layer_offset));
    Write(ui.base,owner+0x11C,Read(ui.base,parent_screen+0x11C));
    Write(ui.base,metadata,owner);
    ui.Call(0x82F219E8,{owner}); GuestPtr(ui.base,owner+0x4C)[0]=1;
    ui.Call(0x82700700,{owner+0x5C,preloader+0x20});
    Write(ui.base,ui.scratch+800,owner);
    ui.Call(0x82985560,{ui_manager+0x24,Read(ui.base,ui_manager+0x24),ui.scratch+800});
    GuestPtr(ui.base,ui_manager+0x5D)[0]=1;
    ui.Call(0x82F293E0,{Read(ui.base,ui_manager+0x0C),runtime});
    ui.Call(0x82F1EA48,{owner,1}); ui.Call(0x82F21950,{owner});
    ui.Call(0x82F1EC20,{owner,Hash("INTRO") & 0x7FFFFFFF});
    // Queue every authored node once, including static paper/material leaves.
    // Loading/Init can leave their world caches unbuilt until explicitly dirty.
    if (!DirtyTree(ui)) { status=Status::Failed; SetVisible(ui,false); return false; }
    status=Status::Ready; SetVisible(ui,false); return true;
  }
  uint32_t Find(Native& ui, std::string_view path) const { return ui.Find(access,root,path); }
  void Layer(Native& ui,uint32_t layer) {
    if(!owner||Read(ui.base,owner+0x110)==layer)return;
    Write(ui.base,owner+0x110,layer);
    const auto manager=Read(ui.base,0x833CAAC0);
    if(manager)GuestPtr(ui.base,manager+0x5D)[0]=1;
  }
  void SetVisible(Native& ui, bool show) {
    visible=show && Ready();
    if (render_root) GuestPtr(ui.base,render_root+0xFC)[0]=uint8_t(visible);
    if (owner) GuestPtr(ui.base,owner+0x144)[0]=uint8_t(visible);
    if (presentation) {
      ui.Call(0x82FEB480,{presentation,uint32_t(!visible)});
      ui.Call(0x82FEB488,{presentation,uint32_t(!visible)});
      const auto manager=ui.Virtual(presentation,0x54,{presentation});
      const auto adapter=ui.Virtual(manager,0x3C,{manager});
      if (adapter) GuestPtr(ui.base,adapter+0x110)[0]=1;
    }
    if (presentation && root) ui.Show(presentation,root,visible);
  }
  uint32_t Components(Native& ui) const { return presentation ? Read(ui.base,presentation+0x20) : 0; }
  uint32_t ControllerByVtable(Native& ui, uint32_t table) const {
    const auto manager=Components(ui);
    if (!manager) return 0;
    const auto begin=Read(ui.base,manager+0x28), end=Read(ui.base,manager+0x2C);
    if (!begin || end<begin || end-begin>8192 || (end-begin)%8) return 0;
    for (auto item=begin; item<end; item+=8) {
      const auto controller=Read(ui.base,item+4);
      if (controller && Read(ui.base,controller)==table) return controller;
    }
    return 0;
  }
  bool Isolate(Native& ui, const std::vector<uint32_t>& retain) {
    if (!Ready() || retain.empty()) return false;
    std::vector<uint32_t> ancestry, excluded;
    for (auto leaf : retain) {
      auto node=leaf;
      unsigned depth=0;
      for (; node && depth++<128; node=Read(ui.base,node+12)) {
        if (std::find(ancestry.begin(),ancestry.end(),node)==ancestry.end()) ancestry.push_back(node);
        if (node==root) break;
      }
      if (node!=root || ancestry.size()>8192) return false;
    }
    for (auto parent : ancestry) {
      if (std::find(retain.begin(),retain.end(),parent)!=retain.end()) continue;
      unsigned count=0;
      for (auto child=Read(ui.base,parent+20); child; child=Read(ui.base,child+16)) {
        if (++count>1024 || excluded.size()>8192) return false;
        const auto name=Read(ui.base,child);
        if (name==Hash("Camera") || name==Hash("Light") ||
            std::find(ancestry.begin(),ancestry.end(),child)!=ancestry.end()) continue;
        excluded.push_back(child);
      }
    }
    retained_ancestry=std::move(ancestry); hidden=std::move(excluded);
    ApplyVisibility(ui); return true;
  }
  void ApplyVisibility(Native& ui) const {
    if (!Ready()) return;
    for (auto node : hidden) ui.Show(presentation,node,false);
    for (auto node : retained_ancestry) if (node!=root) ui.Show(presentation,node,true);
  }
  void Tick(Native& ui, const std::function<void()>& bind = {}) {
    if (!Ready() || !visible || !ui) return;
    ui.Call(0x82FEE208,{presentation});
    ApplyVisibility(ui);
    if (bind) bind();
    ui.Call(0x82FE39A0,{runtime});
    const auto manager=Read(ui.base,presentation+0x14);
    if (manager) {
      const auto clock=ui.Virtual(manager+8,0x10,{manager+8});
      ui.Call(0x82FECE38,{presentation,clock});
    }
  }
  void Release(Native& ui) {
    if (!ui || !preloader) return;
    SetVisible(ui,false);
    if (owner) ui.Call(0x82F2F828,{Read(ui.base,0x833CAAC0),owner,0});
    ui.Call(0x82EB2C98,{preloader+0x50}); ui.Call(0x824CEAB0,{preloader+0x50});
    ui.Call(0x82F27400,{preloader}); ui.Call(0x8242D1A0,{preloader});
    preloader=presentation=access=root=runtime=owner=render_root=0;
    visible=false; status=Status::Idle; retained_ancestry.clear(); hidden.clear();
  }
private:
  bool DirtyTree(Native& ui) const {
    std::vector<uint32_t> pending{root};
    unsigned visited=0;
    while (!pending.empty()) {
      if (++visited>8192) return false;
      const auto node=pending.back(); pending.pop_back();
      ui.Dirty(presentation,node);
      unsigned siblings=0;
      for (auto child=Read(ui.base,node+20); child; child=Read(ui.base,child+16)) {
        if (++siblings>1024 || pending.size()>=8192) return false;
        pending.push_back(child);
      }
    }
    return true;
  }
  void Take(Donor& other) noexcept {
    status=std::exchange(other.status,Status::Idle);
    preloader=std::exchange(other.preloader,0); presentation=std::exchange(other.presentation,0);
    access=std::exchange(other.access,0); root=std::exchange(other.root,0);
    runtime=std::exchange(other.runtime,0); owner=std::exchange(other.owner,0);
    render_root=std::exchange(other.render_root,0); visible=std::exchange(other.visible,false);
    retained_ancestry=std::move(other.retained_ancestry); hidden=std::move(other.hidden);
  }
  bool Filter(Native& ui, const KeepController& keep) {
    const auto components=Read(ui.base,presentation+0x20);
    if (!components) return false;
    for (auto offset : {0x28u,0x38u}) {
      const auto begin=Read(ui.base,components+offset), end=Read(ui.base,components+offset+4);
      if (end<begin || end-begin>8192 || (end-begin)%8 || (!begin && end)) return false;
    }
    for (auto offset : {0x28u,0x38u}) {
      const auto begin=Read(ui.base,components+offset), end=Read(ui.base,components+offset+4);
      auto output=begin;
      for (auto item=begin; item<end; item+=8) {
        const auto wrapper=Read(ui.base,item), controller=Read(ui.base,item+4);
        if (!wrapper || !controller || !keep || !keep(wrapper,controller)) continue;
        Write(ui.base,output,wrapper); Write(ui.base,output+4,controller); output+=8;
      }
      Write(ui.base,components+offset+4,output);
    }
    return true;
  }
  static bool Drain(Native& ui, uint32_t manager) {
    if (!manager) return false;
    if (!Read(ui.base,manager+0x27C)) return true;
    ui.Call(0x82FE3F28,{manager+0x284});
    unsigned drained=0;
    bool valid=true;
    while (const auto count=Read(ui.base,manager+0x27C)) {
      const auto entries=Read(ui.base,manager+0x274);
      if (!entries || count>1024 || drained++>=1024) { valid=false; break; }
      const auto pair=entries+(count-1)*8;
      const auto p=Read(ui.base,pair), r=Read(ui.base,pair+4);
      Write(ui.base,manager+0x27C,count-1);
      if (p && r && ui.Virtual(p,8,{p})) {
        ui.Call(0x82FF2DD8,{manager+0x28,p});
        const auto adapter=Read(ui.base,manager+0x20C);
        if (adapter) ui.Virtual(adapter,8,{adapter,r});
      }
    }
    ui.Call(0x82FE3F50,{manager+0x284});
    if (valid) if (const auto callback=Read(ui.base,manager+0x248))
      ui.Call(callback,{3,0,0,Read(ui.base,manager+0x24C)});
    return valid;
  }
};
} // namespace forza::fh1_menu_widgets
#endif
