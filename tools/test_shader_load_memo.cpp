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
using Memo = memo::ShaderLoadMemo<Shader, 8>;

// A load that must not happen (the memo should have answered).
Shader* NoLoad(const uint32_t*) {
  CHECK(!"unexpected load");
  return nullptr;
}
// The memo's answer for these words, without loading.
Shader* Lookup(Memo& m, const void* owner, uint64_t generation, const uint32_t* p, uint32_t n) {
  return m.Find(owner, generation, p, n);
}
}  // namespace

int main() {
  Memo m;
  Shader a{1}, b{2}, c{3};
  const int owner = 0, other_owner = 0;
  std::vector<uint32_t> memory(4096, 0);
  for (size_t i = 0; i < memory.size(); ++i) memory[i] = uint32_t(i * 2654435761u);
  const uint32_t* p = memory.data() + 64;
  int loads = 0;
  auto load = [&](Shader* s) {
    return [&loads, s](const uint32_t*) {
      ++loads;
      return s;
    };
  };

  // Nothing stored yet: a load.
  CHECK(Lookup(m, &owner, 1, p, 32) == nullptr);
  CHECK(m.FindOrLoad(&owner, 1, p, 32, load(&a)) == &a && loads == 1);
  // The same address, length and words: the stored shader, no load.
  CHECK(m.FindOrLoad(&owner, 1, p, 32, NoLoad) == &a);
  // Another length at the same address (a different shader there).
  CHECK(Lookup(m, &owner, 1, p, 31) == nullptr);
  CHECK(Lookup(m, &owner, 1, p, 33) == nullptr);
  // Another generation (the cache deleted its shaders) or another cache.
  CHECK(Lookup(m, &owner, 2, p, 32) == nullptr);
  CHECK(Lookup(m, &other_owner, 1, p, 32) == nullptr);
  // The guest rewrote any word of it: the hash lookup decides again.
  for (uint32_t i = 0; i < 32; ++i) {
    uint32_t saved = memory[64 + i];
    memory[64 + i] ^= 1u << (i % 32);
    CHECK(Lookup(m, &owner, 1, p, 32) == nullptr);
    memory[64 + i] = saved;
  }
  CHECK(Lookup(m, &owner, 1, p, 32) == &a);
  // A word just past the stored length does not matter.
  memory[64 + 32] ^= 0xFFFFFFFFu;
  CHECK(Lookup(m, &owner, 1, p, 32) == &a);

  // The load gets the memo's copy of the words, and a guest write during the load
  // (after the copy) neither changes that copy nor makes the memo pair the new
  // words with the shader made from the old ones.
  {
    std::vector<uint32_t> before(memory.begin() + 200, memory.begin() + 240);
    const uint32_t* q = memory.data() + 200;
    Shader* got = m.FindOrLoad(&owner, 1, q, 40, [&](const uint32_t* words) {
      CHECK(words != q);
      for (int i = 0; i < 40; ++i) memory[200 + i] ^= 0x5A5A5A5Au;  // the guest writes
      for (int i = 0; i < 40; ++i) CHECK(words[i] == before[i]);    // the copy holds
      return &b;
    });
    CHECK(got == &b);
    // The guest memory now holds other words: a miss, so the next load hashes them.
    CHECK(Lookup(m, &owner, 1, q, 40) == nullptr);
    CHECK(m.FindOrLoad(&owner, 1, q, 40, load(&c)) == &c);
    // Back to the words b was made from: b's entry was replaced by c's, so a load again.
    for (int i = 0; i < 40; ++i) memory[200 + i] = before[i];
    CHECK(Lookup(m, &owner, 1, q, 40) == nullptr);
  }

  // A new load at the address replaces the slot's shader and words.
  CHECK(m.FindOrLoad(&owner, 1, p, 16, load(&b)) == &b);
  CHECK(Lookup(m, &owner, 1, p, 16) == &b);
  CHECK(Lookup(m, &owner, 1, p, 32) == nullptr);
  // Same words at another address: a separate entry (the key is the address).
  std::vector<uint32_t> copy(memory.begin() + 64, memory.begin() + 80);
  CHECK(Lookup(m, &owner, 1, copy.data(), 16) == nullptr);
  // Many addresses share 8 slots: a slot taken by another address is a miss, never a
  // wrong shader.
  std::vector<const uint32_t*> addresses;
  for (int i = 0; i < 64; ++i) addresses.push_back(memory.data() + 300 + i * 40);
  for (int i = 0; i < 64; ++i) m.FindOrLoad(&owner, 1, addresses[i], 20, load(i % 2 ? &b : &c));
  int hits = 0;
  for (int i = 0; i < 64; ++i) {
    Shader* found = Lookup(m, &owner, 1, addresses[i], 20);
    if (found) {
      ++hits;
      CHECK(found == (i % 2 ? &b : &c));
    }
  }
  CHECK(hits > 0 && hits <= 8);
  // A zero-length load matches only a zero-length entry.
  CHECK(m.FindOrLoad(&owner, 1, p + 3000, 0, load(&a)) == &a);
  CHECK(Lookup(m, &owner, 1, p + 3000, 0) == &a);
  CHECK(Lookup(m, &owner, 1, p + 3000, 1) == nullptr);
  std::printf("shader load memo: ok (%d of 64 colliding addresses kept)\n", hits);
  return 0;
}
