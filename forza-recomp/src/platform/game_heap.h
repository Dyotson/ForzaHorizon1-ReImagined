// forza - the game's own heaps and the movies' fallback blocks (#262, src/hooks/movie_memory.cpp).
//
// The game's memory manager (sub_823F7808, its object at 0x832E1360) keeps eight arenas of
// 0x314 bytes from 0x832AAC24 (TU4 0x8339664C); each holds two heaps, category 0x1N at
// arena+0x10 and category 0x2N at arena+0x190 (sub_82CBBDA0), used when the arena's byte
// +0x310 is set and the heap's size (+0x34) is not 0. A heap (alloc sub_823FCCA8) keeps:
//   +0x34 size, +0x38 the most it had in use, +0x44 bytes in use, +0x4C failed allocations,
//   +0x50 failed allocations that would have fitted in its free bytes (fragmentation),
//   +0x68 its free blocks: a std::map (MSVC) head node; node +0 left, +4 parent, +8 right,
//   +0x10 the block (+0 address, +8 size), byte +0x15 "is nil" (the head is the nil node),
//   +0x158 its name (a C string; sub_82CBBE18): category 0x20 is "streamingGPU".
// Everything is big-endian guest memory; the reader below takes a callback for it so the
// walk can be tested without a guest.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace forza::game_heap {

constexpr uint32_t kArenaSize = 0x314;
constexpr uint32_t kArenaEnabled = 0x310;  // byte
constexpr uint32_t kLowHeap = 0x10, kHighHeap = 0x190;
constexpr uint32_t kSize = 0x34, kPeak = 0x38, kUsed = 0x44, kFailed = 0x4C,
                   kFailedFragmented = 0x50, kFreeTree = 0x68, kName = 0x158;
constexpr uint32_t kNodeLeft = 0x0, kNodeParent = 0x4, kNodeRight = 0x8, kNodeBlock = 0x10,
                   kNodeIsNil = 0x15;
constexpr uint32_t kBlockAddress = 0x0, kBlockSize = 0x8;
// A heap with more free blocks than this is not walked to the end (the tree would be damaged,
// or the walk raced the game).
constexpr uint32_t kMaxNodes = 1u << 16;

// The heap object of a category (0x1N or 0x2N, N < 8) in the arena table at `arenas`; 0 for
// another category. Whether the heap is in use is the caller's question (kArenaEnabled, kSize).
constexpr uint32_t HeapOfCategory(uint32_t arenas, uint32_t category) {
  const uint32_t arena = category & 0xF, kind = category & 0x30;
  if (arena >= 8 || (kind != 0x10 && kind != 0x20)) return 0;
  return arenas + arena * kArenaSize + (kind == 0x10 ? kLowHeap : kHighHeap);
}

struct Census {
  uint32_t size = 0, used = 0, peak = 0, failed = 0, failed_fragmented = 0;
  uint32_t free_blocks = 0, free_bytes = 0, largest_free = 0;
  bool complete = false;  // the walk reached the end of the free-block tree
};

// Reads a heap's counters and walks its free blocks in order (from the leftmost node, by
// successor, as sub_82CBC470 does). `read32(address, &value)` and `read8(address, &value)`
// return false for memory that cannot be read, which ends the walk (complete stays false).
template <typename Read32, typename Read8>
Census Count(uint32_t heap, Read32 read32, Read8 read8) {
  Census c;
  uint32_t head = 0, node = 0;
  if (!read32(heap + kSize, &c.size) || !read32(heap + kPeak, &c.peak) ||
      !read32(heap + kUsed, &c.used) || !read32(heap + kFailed, &c.failed) ||
      !read32(heap + kFailedFragmented, &c.failed_fragmented) ||
      !read32(heap + kFreeTree, &head) || !head || !read32(head + kNodeLeft, &node))
    return c;
  auto is_nil = [&](uint32_t n, bool* nil) {
    uint8_t v = 0;
    if (!n || !read8(n + kNodeIsNil, &v)) return false;
    *nil = v != 0;
    return true;
  };
  for (uint32_t count = 0; count <= kMaxNodes; ++count) {
    bool nil = false;
    if (!is_nil(node, &nil)) return c;
    if (nil) {  // back at the head: every node was visited
      c.complete = node == head;
      return c;
    }
    uint32_t block = 0, size = 0;
    if (!read32(node + kNodeBlock, &block) || !read32(block + kBlockSize, &size)) return c;
    ++c.free_blocks;
    c.free_bytes += size;
    if (size > c.largest_free) c.largest_free = size;
    // successor: the leftmost node of the right subtree, else the first ancestor reached
    // from its left
    uint32_t right = 0;
    if (!read32(node + kNodeRight, &right) || !is_nil(right, &nil)) return c;
    if (!nil) {
      node = right;
      for (uint32_t depth = 0;; ++depth) {
        uint32_t left = 0;
        if (depth > kMaxNodes || !read32(node + kNodeLeft, &left) || !is_nil(left, &nil))
          return c;
        if (nil) break;
        node = left;
      }
    } else {
      uint32_t parent = 0;
      for (uint32_t depth = 0;; ++depth) {
        if (depth > kMaxNodes || !read32(node + kNodeParent, &parent) || !is_nil(parent, &nil))
          return c;
        uint32_t parent_right = 0;
        if (nil || !read32(parent + kNodeRight, &parent_right) || parent_right != node) break;
        node = parent;
      }
      node = parent;
    }
  }
  return c;
}

// XMemAlloc attributes as the game reads them: bit 31 physical memory, bit 30 zero the block,
// bits 28-29 the memory protection, bits 24-27 the alignment.
constexpr bool IsPhysical(uint32_t attributes) { return attributes >> 31; }
constexpr bool ZeroInitialize(uint32_t attributes) { return (attributes >> 30) & 1; }

// The page protection the movie allocator (sub_82E5B7C8) asks physical memory for:
// write-combined large pages, no cache, read/write, write-combined.
constexpr uint32_t PhysicalProtect(uint32_t attributes) {
  switch ((attributes >> 28) & 3) {
    case 0: return 0x20000404;
    case 1: return 0x204;
    case 3: return 0x404;
    default: return 0x4;
  }
}

// Its alignment for physical memory (1 << the field, 4 KB for 0), at least a page here: the
// block replaces one the streaming heap would have given page aligned.
constexpr uint32_t PhysicalAlignment(uint32_t attributes) {
  const uint32_t field = (attributes >> 24) & 0xF;
  return field > 12 ? 1u << field : 0x1000;
}

// The blocks a movie got from physical memory in place of the streaming heap: XMemFree and
// XMemSize must not take them for the general heap's. Empty() is lock-free for their fast path.
class FallbackBlocks {
 public:
  void Add(uint32_t address, uint32_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (blocks_.emplace(address, size).second) count_.fetch_add(1, std::memory_order_release);
  }
  // Removes the block; false for an address that is not one.
  bool Take(uint32_t address) {
    if (Empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!blocks_.erase(address)) return false;
    count_.fetch_sub(1, std::memory_order_release);
    return true;
  }
  // The block's size, or 0 for an address that is not one.
  uint32_t Size(uint32_t address) const {
    if (Empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = blocks_.find(address);
    return it == blocks_.end() ? 0 : it->second;
  }
  bool Empty() const { return count_.load(std::memory_order_acquire) == 0; }

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, uint32_t> blocks_;
  std::atomic<uint32_t> count_{0};
};

}  // namespace forza::game_heap
