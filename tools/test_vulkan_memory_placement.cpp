// Where the Vulkan backend puts new memory on integrated GPUs (patch 0138, #268): the policy
// in src/graphics/vulkan/fh1_memory_placement.h of the patched SDK source.
// Build with the patched SDK source's src/graphics/vulkan on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/graphics/vulkan
//       tools/test_vulkan_memory_placement.cpp -o test_vulkan_memory_placement
#include "fh1_memory_placement.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace p = rex::graphics::vulkan::fh1_memory_placement;

#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      std::fprintf(stderr, "memory placement: FAIL: %s\n", #x);    \
      std::exit(1);                                                 \
    }                                                               \
  } while (false)

namespace {

constexpr uint64_t kMB = uint64_t(1) << 20;
constexpr uint64_t kGB = uint64_t(1) << 30;
constexpr uint32_t DL = p::kPropertyDeviceLocal, HV = p::kPropertyHostVisible,
                   HC = p::kPropertyHostCoherent, CACHED = p::kPropertyHostCached;
constexpr uint32_t kAmdCoherent = 0x40, kAmdUncached = 0x80, kLazy = 0x10;

struct Builder {
  p::MemoryInfo m;
  explicit Builder(uint32_t device_type) { m.device_type = device_type; }
  Builder& Heap(uint64_t size, bool device_local) {
    m.heap_size[m.heap_count] = size;
    m.heap_flags[m.heap_count] = device_local ? p::kHeapDeviceLocal : 0;
    ++m.heap_count;
    return *this;
  }
  Builder& Type(uint32_t heap, uint32_t flags) {
    m.type_heap[m.type_count] = heap;
    m.type_flags[m.type_count] = flags;
    ++m.type_count;
    return *this;
  }
};

// A Ryzen APU on Windows (#268's log: a 486 MB budget on a 512 MB carve-out).
p::MemoryInfo AmdApuWindows() {
  return Builder(p::kDeviceTypeIntegrated)
      .Heap(512 * kMB, true)
      .Heap(14 * kGB, false)
      .Type(0, DL)
      .Type(1, HV | HC)
      .Type(0, DL | HV | HC)
      .Type(1, HV | HC | CACHED)
      .Type(0, DL | kAmdCoherent | kAmdUncached)
      .Type(1, HV | HC | kAmdCoherent | kAmdUncached)
      .m;
}

// This test machine: RADV, RX 9060 XT (the system heap first, resizable BAR).
p::MemoryInfo RadvDiscrete() {
  return Builder(p::kDeviceTypeDiscrete)
      .Heap(15 * kGB, false)
      .Heap(15 * kGB, true)
      .Type(1, DL)
      .Type(1, DL)
      .Type(0, HV | HC)
      .Type(1, DL | HV | HC)
      .Type(1, DL | HV | HC)
      .Type(0, HV | HC | CACHED)
      .Type(0, HV | HC | CACHED)
      .m;
}

}  // namespace

int main() {
  // The layouts.
  {
    const p::Layout l = p::Describe(AmdApuWindows());
    CHECK(l.integrated);
    CHECK(l.device_local_types == 0b101);  // not the AMD device-coherent type 4
    CHECK(l.device_local_heaps == 0b1);
    CHECK(l.device_local_bytes == 512 * kMB);
    CHECK(l.system_heap == 1);
    CHECK(l.system_types == 0b10);  // write-combined: not the device-coherent type 5
    CHECK(l.system_types_cached == 0b1000);
  }
  {
    const p::Layout l = p::Describe(RadvDiscrete());
    CHECK(!l.integrated);  // discrete GPUs keep everything device-local
    CHECK(l.can_move());   // (the test cap still can move memory there)
    CHECK(l.system_heap == 0);
    CHECK(l.device_local_heaps == 0b10);
    CHECK(l.system_types == 0b100);
    CHECK(l.system_types_cached == 0b1100000);
    CHECK(l.device_local_types == 0b11011);
  }
  // A discrete GPU with little VRAM: still discrete.
  CHECK(!p::Describe(Builder(p::kDeviceTypeDiscrete)
                         .Heap(2 * kGB, true)
                         .Heap(32 * kGB, false)
                         .Type(0, DL)
                         .Type(1, HV | HC)
                         .m)
             .integrated);
  // An integrated GPU with a single (device-local) heap, as Intel's: nothing to move to.
  {
    const p::Layout l = p::Describe(Builder(p::kDeviceTypeIntegrated)
                                        .Heap(16 * kGB, true)
                                        .Type(0, DL)
                                        .Type(0, DL | HV | HC)
                                        .Type(0, DL | HV | HC | CACHED)
                                        .m);
    CHECK(!l.integrated && !l.can_move());
  }
  // Another device type with a small device-local heap counts as integrated, a large one not;
  // a CPU device never.
  {
    Builder small(0);
    small.Heap(1 * kGB, true).Heap(8 * kGB, false).Type(0, DL).Type(1, HV | HC);
    CHECK(p::Describe(small.m).integrated);
    Builder large(3);
    large.Heap(4 * kGB, true).Heap(8 * kGB, false).Type(0, DL).Type(1, HV | HC);
    CHECK(!p::Describe(large.m).integrated);
    Builder cpu(p::kDeviceTypeCpu);
    cpu.Heap(1 * kGB, true).Heap(8 * kGB, false).Type(0, DL).Type(1, HV | HC);
    CHECK(!p::Describe(cpu.m).integrated);
  }
  // A system heap whose only type is lazily allocated is no place to move to.
  CHECK(!p::Describe(Builder(p::kDeviceTypeIntegrated)
                         .Heap(512 * kMB, true)
                         .Heap(8 * kGB, false)
                         .Type(0, DL)
                         .Type(1, kLazy)
                         .m)
             .can_move());
  // The largest of two system heaps.
  {
    const p::Layout l = p::Describe(Builder(p::kDeviceTypeIntegrated)
                                        .Heap(256 * kMB, false)
                                        .Heap(512 * kMB, true)
                                        .Heap(8 * kGB, false)
                                        .Type(1, DL)
                                        .Type(0, HV | HC)
                                        .Type(2, HV | HC)
                                        .m);
    CHECK(l.system_heap == 2 && l.system_types == 0b100);
  }

  // The budget the code sees: the driver's, or at most the test cap.
  CHECK(p::CappedBudget(15 * kGB, 0) == 15 * kGB);
  CHECK(p::CappedBudget(15 * kGB, 486) == 486 * kMB);
  CHECK(p::CappedBudget(300 * kMB, 486) == 300 * kMB);
  CHECK(p::CappedBudget(0, 486) == 486 * kMB);

  // VMA's new blocks: an eighth of a heap up to 1 GB, else 256 MB.
  CHECK(p::AllocatorBlockBytes(512 * kMB) == 64 * kMB);
  CHECK(p::AllocatorBlockBytes(1 * kGB) == 128 * kMB);
  CHECK(p::AllocatorBlockBytes(15 * kGB) == 256 * kMB);
  CHECK(p::AllocatorBlockBytes(0) == 256 * kMB);

  // Shares of the budget: textures to 80 %, render targets and guest memory to 95 %.
  {
    const uint64_t budget = 486 * kMB;  // 80 %: 388.8 MB, 95 %: 461.7 MB
    CHECK(p::Fits(p::Kind::kTexture, 300 * kMB, 64 * kMB, budget));
    CHECK(!p::Fits(p::Kind::kTexture, 330 * kMB, 64 * kMB, budget));
    CHECK(p::Fits(p::Kind::kHot, 330 * kMB, 64 * kMB, budget));
    CHECK(p::Fits(p::Kind::kHot, 440 * kMB, 16 * kMB, budget));
    CHECK(!p::Fits(p::Kind::kHot, 450 * kMB, 16 * kMB, budget));
    CHECK(p::Fits(p::Kind::kTexture, 100 * kGB, 1 * kGB, 0));  // no budget known
  }

  // The order of memory types.
  {
    const p::Layout l = p::Describe(AmdApuWindows());
    p::Order o = p::Choose(l, 0xFF, true);
    CHECK(!o.system_first && o.count == 3);
    CHECK(o.masks[0] == 0b101 && o.masks[1] == 0b10 && o.masks[2] == 0b1000);
    o = p::Choose(l, 0xFF, false);
    CHECK(o.system_first && o.count == 3);
    CHECK(o.masks[0] == 0b10 && o.masks[1] == 0b1000 && o.masks[2] == 0b101);
    // An image that takes only device-local types stays there.
    o = p::Choose(l, 0b101, false);
    CHECK(!o.system_first && o.count == 1 && o.masks[0] == 0b101);
    // One that takes only the cached system type: that, then device-local.
    o = p::Choose(l, 0b1001, false);
    CHECK(o.system_first && o.count == 2 && o.masks[0] == 0b1000 && o.masks[1] == 0b1);
    // The AMD-specific types are never chosen.
    o = p::Choose(l, 0b110000, false);
    CHECK(o.count == 0);
    CHECK(p::LowestType(0b1000) == 3 && p::LowestType(0) == UINT32_MAX);
  }

  // A session on the APU: textures, render targets and guest memory pages arrive while the
  // device-local use is tracked; device-local memory never passes the hot share by more than
  // one allocation, textures stop at 80 %, and everything is placed somewhere.
  {
    const p::Layout l = p::Describe(AmdApuWindows());
    const uint64_t budget = 486 * kMB;
    const uint64_t block = p::AllocatorBlockBytes(512 * kMB);
    uint64_t device_local = 120 * kMB;  // the driver's own, the EDRAM buffer, the swap chain
    uint64_t system = 0, textures_device_local = 0;
    uint32_t moved_textures = 0, moved_hot = 0;
    for (int i = 0; i < 400; ++i) {
      const bool texture = i % 3 != 0;
      const uint64_t size = texture ? 6 * kMB : (i % 2 ? 16 * kMB : 4 * kMB);
      const uint64_t growth = texture ? (size > block ? size : block) : size;
      const p::Kind kind = texture ? p::Kind::kTexture : p::Kind::kHot;
      const p::Order o = p::Choose(l, 0xFF, p::Fits(kind, device_local, growth, budget));
      CHECK(o.count > 0);
      if (o.masks[0] & l.device_local_types) {
        // VMA fills its block before it makes the next one: count a texture's block at once.
        if (texture) {
          if (textures_device_local % block == 0) device_local += block;
          textures_device_local += size;
        } else {
          device_local += size;
        }
      } else {
        system += size;
        ++(texture ? moved_textures : moved_hot);
      }
      CHECK(device_local * 100 <= budget * p::kHotPercent + 16 * kMB * 100);
    }
    CHECK(moved_textures > 0 && moved_hot > 0 && system > 0);
    CHECK(device_local <= budget);
  }
  std::puts("memory placement: ok");
  return 0;
}
