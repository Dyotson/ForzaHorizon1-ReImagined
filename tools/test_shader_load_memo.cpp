// The command processor's memo of the shaders it loaded from guest memory (SDK patch
// 0140, #45): src/graphics/vulkan/fh1_shader_load_memo.h of the patched SDK source.
// Build with the patched SDK source's src/graphics/vulkan on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/graphics/vulkan
//       tools/test_shader_load_memo.cpp -o test_shader_load_memo
#include "fh1_shader_load_memo.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace memo = rex::graphics::vulkan::fh1_shader_load_memo;

#define CHECK(x)                                                   \
  do {                                                             \
    if (!(x)) {                                                    \
      std::fprintf(stderr, "shader load memo: FAIL: %s\n", #x);    \
      std::exit(1);                                                \
    }                                                              \
  } while (false)

namespace {
struct Shader {
  int id;
};
}  // namespace

int main() {
  memo::ShaderLoadMemo<Shader, 8> m;
  Shader a{1}, b{2}, c{3};
  const int owner = 0, other_owner = 0;
  std::vector<uint32_t> memory(4096, 0);
  for (size_t i = 0; i < memory.size(); ++i) memory[i] = uint32_t(i * 2654435761u);
  const uint32_t* p = memory.data() + 64;

  // Nothing stored yet.
  CHECK(m.Find(&owner, 1, p, 32) == nullptr);
  m.Store(&owner, 1, p, 32, &a);
  // The same address, length and words: the stored shader.
  CHECK(m.Find(&owner, 1, p, 32) == &a);
  // Another length at the same address (a different shader there).
  CHECK(m.Find(&owner, 1, p, 31) == nullptr);
  CHECK(m.Find(&owner, 1, p, 33) == nullptr);
  // Another generation (the cache deleted its shaders) or another cache.
  CHECK(m.Find(&owner, 2, p, 32) == nullptr);
  CHECK(m.Find(&other_owner, 1, p, 32) == nullptr);
  // The guest rewrote any word of it: the hash lookup decides again.
  for (uint32_t i = 0; i < 32; ++i) {
    uint32_t saved = memory[64 + i];
    memory[64 + i] ^= 1u << (i % 32);
    CHECK(m.Find(&owner, 1, p, 32) == nullptr);
    memory[64 + i] = saved;
  }
  CHECK(m.Find(&owner, 1, p, 32) == &a);
  // A word just past the stored length does not matter.
  memory[64 + 32] ^= 0xFFFFFFFFu;
  CHECK(m.Find(&owner, 1, p, 32) == &a);
  // Storing again replaces the slot's shader and words.
  m.Store(&owner, 1, p, 16, &b);
  CHECK(m.Find(&owner, 1, p, 16) == &b);
  CHECK(m.Find(&owner, 1, p, 32) == nullptr);
  // Same words at another address: a separate entry (the key is the address).
  std::vector<uint32_t> copy(memory.begin() + 64, memory.begin() + 80);
  CHECK(m.Find(&owner, 1, copy.data(), 16) == nullptr);
  // Many addresses share 8 slots: a slot taken by another address is a miss, never a
  // wrong shader.
  std::vector<const uint32_t*> addresses;
  for (int i = 0; i < 64; ++i) addresses.push_back(memory.data() + 128 + i * 40);
  for (int i = 0; i < 64; ++i) m.Store(&owner, 1, addresses[i], 20, i % 2 ? &b : &c);
  int hits = 0;
  for (int i = 0; i < 64; ++i) {
    Shader* found = m.Find(&owner, 1, addresses[i], 20);
    if (found) {
      ++hits;
      CHECK(found == (i % 2 ? &b : &c));
    }
  }
  CHECK(hits > 0 && hits <= 8);
  // A zero-length load matches only a zero-length entry.
  m.Store(&owner, 1, p + 1000, 0, &a);
  CHECK(m.Find(&owner, 1, p + 1000, 0) == &a);
  CHECK(m.Find(&owner, 1, p + 1000, 1) == nullptr);
  std::printf("shader load memo: ok (%d of 64 colliding addresses kept)\n", hits);
  return 0;
}
