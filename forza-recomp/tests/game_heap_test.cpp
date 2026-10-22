// The game's heaps and the movies' fallback blocks (src/platform/game_heap.h, #262): which heap
// a category is, the walk of a heap's free blocks (an MSVC std::map in big-endian memory), the
// physical memory a movie block takes in place of the streaming heap, and the set of those.
// Standalone: c++ -std=c++17 game_heap_test.cpp -o game_heap_test
#include "../src/platform/game_heap.h"
#include <cassert>
#include <cstdint>
#include <map>
#include <thread>
#include <vector>

namespace h = forza::game_heap;

struct Guest {
  std::map<uint32_t, uint8_t> bytes;
  void Put32(uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes[a + i] = uint8_t(v >> (24 - 8 * i));
  }
  void Put8(uint32_t a, uint8_t v) { bytes[a] = v; }
  bool Read32(uint32_t a, uint32_t* v) const {
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) {
      auto it = bytes.find(a + i);
      if (it == bytes.end()) return false;
      r = r << 8 | it->second;
    }
    *v = r;
    return true;
  }
  bool Read8(uint32_t a, uint8_t* v) const {
    auto it = bytes.find(a);
    if (it == bytes.end()) return false;
    *v = it->second;
    return true;
  }
  h::Census Count(uint32_t heap) const {
    return h::Count(
        heap, [this](uint32_t a, uint32_t* v) { return Read32(a, v); },
        [this](uint32_t a, uint8_t* v) { return Read8(a, v); });
  }
};

constexpr uint32_t kHeap = 0x832AADB4, kHead = 0x40001000;

// A node at `n` with children `l`, `r` (kHead for none), parent `p` and a block of `size`.
void Node(Guest& g, uint32_t n, uint32_t l, uint32_t p, uint32_t r, uint32_t size) {
  g.Put32(n + h::kNodeLeft, l);
  g.Put32(n + h::kNodeParent, p);
  g.Put32(n + h::kNodeRight, r);
  g.Put8(n + h::kNodeIsNil, 0);
  const uint32_t block = n + 0x800;
  g.Put32(n + h::kNodeBlock, block);
  g.Put32(block + h::kBlockAddress, 0x50000000 + n);
  g.Put32(block + h::kBlockSize, size);
}

Guest HeapWithHead(uint32_t leftmost, uint32_t root, uint32_t rightmost) {
  Guest g;
  g.Put32(kHeap + h::kSize, 48u << 20);
  g.Put32(kHeap + h::kPeak, 47u << 20);
  g.Put32(kHeap + h::kUsed, 45u << 20);
  g.Put32(kHeap + h::kFailed, 1);
  g.Put32(kHeap + h::kFailedFragmented, 1);
  g.Put32(kHeap + h::kFreeTree, kHead);
  g.Put32(kHead + h::kNodeLeft, leftmost);
  g.Put32(kHead + h::kNodeParent, root);
  g.Put32(kHead + h::kNodeRight, rightmost);
  g.Put8(kHead + h::kNodeIsNil, 1);
  return g;
}

int main() {
  // sub_82CBBDA0: arena (category & 0xF) * 0x314, heap +0x10 for 0x1N, +0x190 for 0x2N.
  constexpr uint32_t arenas = 0x832AAC24;
  static_assert(h::HeapOfCategory(arenas, 0x20) == 0x832AADB4, "");
  static_assert(h::HeapOfCategory(arenas, 0x10) == 0x832AAC34, "");
  static_assert(h::HeapOfCategory(arenas, 0x21) == 0x832AADB4 + 0x314, "");
  static_assert(h::HeapOfCategory(arenas, 0x27) == 0x832AADB4 + 7 * 0x314, "");
  static_assert(h::HeapOfCategory(arenas, 0x28) == 0, "");  // 8: no heap
  static_assert(h::HeapOfCategory(arenas, 0x00) == 0, "");
  static_assert(h::HeapOfCategory(arenas, 0x30) == 0, "");

  // No free block: the head is its own leftmost node.
  {
    Guest g = HeapWithHead(kHead, kHead, kHead);
    h::Census c = g.Count(kHeap);
    assert(c.complete && c.free_blocks == 0 && c.free_bytes == 0 && c.largest_free == 0);
    assert(c.size == 48u << 20 && c.used == 45u << 20 && c.peak == 47u << 20);
    assert(c.failed == 1 && c.failed_fragmented == 1);
  }

  // B(30) has A(10) on its left and D(40) on its right; A has F(5) on its right, D has C(25)
  // on its left.
  // In order: A F B C D. Every successor case: right subtree, climbing from a right child
  // (F -> B), from a left child (C -> D), and from the rightmost node to the head.
  constexpr uint32_t A = 0x40002000, B = 0x40003000, C = 0x40004000, D = 0x40005000,
                     F = 0x40006000;
  {
    Guest g = HeapWithHead(A, B, D);
    Node(g, B, A, kHead, D, 30 << 10);
    Node(g, A, kHead, B, F, 10 << 10);
    Node(g, F, kHead, A, kHead, 5 << 10);
    Node(g, D, C, B, kHead, 40 << 10);
    Node(g, C, kHead, D, kHead, 25 << 10);
    h::Census c = g.Count(kHeap);
    assert(c.complete);
    assert(c.free_blocks == 5);
    assert(c.free_bytes == (110u << 10));
    assert(c.largest_free == (40u << 10));

    // A block that cannot be read ends the walk with what was counted.
    Guest cut = g;
    cut.bytes.erase(C + 0x800 + h::kBlockSize);
    h::Census d = cut.Count(kHeap);
    assert(!d.complete && d.free_blocks == 3 && d.largest_free == (30u << 10));

    // A damaged tree that loops (D's left child is B again) stops after kMaxNodes.
    Guest loop = g;
    loop.Put32(D + h::kNodeLeft, B);
    loop.Put32(B + h::kNodeLeft, kHead);
    h::Census e = loop.Count(kHeap);
    assert(!e.complete);
  }

  // A heap whose fields cannot be read gives an empty, incomplete census.
  {
    Guest g;
    h::Census c = g.Count(kHeap);
    assert(!c.complete && c.size == 0 && c.free_blocks == 0);
  }

  // The decoder's frame buffer that did not fit (XMediaFacade sub_8815B9F8): XMV's allocator
  // id 0x8C, read/write, 16-byte alignment, not physical, not zeroed.
  constexpr uint32_t frame = 0x248C8001;
  static_assert(!h::IsPhysical(frame) && !h::ZeroInitialize(frame), "");
  static_assert(h::PhysicalProtect(frame) == 0x4, "");    // PAGE_READWRITE
  static_assert(h::PhysicalAlignment(frame) == 0x1000, "");  // a page, never less
  static_assert(h::IsPhysical(0x80000000) && h::ZeroInitialize(0x40000000), "");
  // sub_82E5B7C8's protections for the other fields; alignments above a page are kept.
  static_assert(h::PhysicalProtect(0x00000000) == 0x20000404, "");
  static_assert(h::PhysicalProtect(0x10000000) == 0x204, "");
  static_assert(h::PhysicalProtect(0x30000000) == 0x404, "");
  static_assert(h::PhysicalAlignment(0x0C000000) == 0x1000, "");
  static_assert(h::PhysicalAlignment(0x0D000000) == 0x2000, "");
  static_assert(h::PhysicalAlignment(0x0F000000) == 0x8000, "");

  // The fallback blocks: XMemFree takes each one once, XMemSize answers for it, anything else
  // is the game's.
  {
    h::FallbackBlocks blocks;
    assert(blocks.Empty() && !blocks.Take(0xA1000000) && blocks.Size(0xA1000000) == 0);
    blocks.Add(0xA1000000, 2826496);
    blocks.Add(0xA1300000, 6912000);
    assert(!blocks.Empty());
    assert(blocks.Size(0xA1000000) == 2826496 && blocks.Size(0xA1300000) == 6912000);
    assert(blocks.Size(0xA1000004) == 0 && !blocks.Take(0xA1000004));
    assert(blocks.Take(0xA1000000) && !blocks.Take(0xA1000000));
    assert(blocks.Size(0xA1000000) == 0 && !blocks.Empty());
    blocks.Add(0xA1300000, 1);  // already there: kept as it was
    assert(blocks.Size(0xA1300000) == 6912000);
    assert(blocks.Take(0xA1300000) && blocks.Empty());

    // Threads adding and taking their own blocks leave it empty.
    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 4; ++t)
      threads.emplace_back([&blocks, t] {
        for (uint32_t i = 0; i < 1000; ++i) {
          const uint32_t a = 0xA0000000 + (t << 24) + (i << 12);
          blocks.Add(a, 0x10000);
          assert(blocks.Size(a) == 0x10000);
          assert(blocks.Take(a));
        }
      });
    for (auto& t : threads) t.join();
    assert(blocks.Empty());
  }
}
