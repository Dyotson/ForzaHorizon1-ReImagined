// The pipeline lookahead's walk over queued commands (SDK patch 0270, #378):
// src/graphics/vulkan/fh1_pipeline_lookahead.h of the patched SDK source.
// Build with the patched SDK source's src/graphics/vulkan on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/graphics/vulkan
//       tools/test_pipeline_lookahead.cpp -o test_pipeline_lookahead
#include "fh1_pipeline_lookahead.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace la = rex::graphics::vulkan::fh1_pipeline_lookahead;

#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::fprintf(stderr, "pipeline lookahead: FAIL: %s (line %d)\n", #x, __LINE__); \
      std::exit(1);                                                  \
    }                                                                \
  } while (false)

namespace {

// Guest words are big-endian: a buffer of host values as the guest stores them.
struct Words {
  std::vector<uint32_t> w;
  Words& operator<<(uint32_t v) {
    w.push_back(la::Swap32(v));
    return *this;
  }
  uint32_t size() const { return uint32_t(w.size()); }
};

uint32_t Type0(uint32_t index, uint32_t count, bool one = false) {
  return (uint32_t(count - 1) << 16) | (one ? 0x8000u : 0) | index;
}
uint32_t Type3(uint32_t opcode, uint32_t count, bool predicated = false) {
  return 0xC0000000u | (uint32_t(count - 1) << 16) | (opcode << 8) | (predicated ? 1u : 0u);
}
constexpr uint32_t kType2 = 0x80000000u;

struct Draw {
  uint32_t initiator;
  bool dma;
  uint32_t base, size;
  std::map<uint32_t, uint32_t> registers;  // as the hooks saw them at the draw
};

struct Hooks {
  std::map<uint32_t, uint32_t> registers;
  std::map<uint32_t, Words*> memory;  // guest physical address -> buffer
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> shaders;  // type, guest-endian words
  std::vector<Draw> draws;
  uint32_t swaps = 0;
  la::End draw_result = la::End::kQueueEnd;
  uint32_t draw_result_at = UINT32_MAX;  // the draw (0-based) that returns draw_result

  void Register(uint32_t index, uint32_t value) {
    CHECK(index >= la::kFirstRegister && index < la::kFirstRegister + la::kRegisterCount);
    registers[index] = value;
  }
  const uint32_t* Translate(uint32_t physical, uint32_t bytes) {
    for (auto& [address, words] : memory) {
      if (physical >= address && physical + bytes <= address + words->size() * 4) {
        return words->w.data() + (physical - address) / 4;
      }
    }
    return nullptr;
  }
  void LoadShader(uint32_t type, const uint32_t* words, uint32_t dwords) {
    shaders.emplace_back(type, std::vector<uint32_t>(words, words + dwords));
  }
  la::End Draw(uint32_t initiator, bool dma, uint32_t base, uint32_t size) {
    draws.push_back({initiator, dma, base, size, registers});
    return draws.size() - 1 == draw_result_at ? draw_result : la::End::kQueueEnd;
  }
  la::End Swap() {
    ++swaps;
    return la::End::kQueueEnd;
  }
  // The time limit: reached once Out() has been asked more than out_after times.
  uint32_t out_calls = 0;
  uint32_t out_after = UINT32_MAX;
  bool Out() { return ++out_calls > out_after; }
};

la::End WalkAll(Hooks& hooks, const Words& words, uint32_t start = 0, uint32_t count = UINT32_MAX,
                uint32_t wrap = 0, uint32_t max_packets = 1000,
                la::Walker<Hooks>** walker_out = nullptr) {
  static la::Walker<Hooks>* walker = nullptr;
  delete walker;
  walker = new la::Walker<Hooks>(hooks, max_packets, 0xFFFFFFFFull, 0xFFFFFFFFull);
  if (walker_out) *walker_out = walker;
  return walker->Walk(words.w.data(), start, count == UINT32_MAX ? words.size() : count, wrap);
}

}  // namespace

int main() {
  // Register writes: type 0 (a range and one register), SET_CONSTANT type 4, SET_CONSTANT2;
  // only the context registers reach the hooks.
  {
    Hooks hooks;
    Words c;
    c << Type0(0x2000, 2) << 11 << 12;                 // RB_SURFACE_INFO, +1
    c << Type0(0x2100, 3, true) << 1 << 2 << 3;        // one register, last write wins
    c << Type0(0x0578, 1) << 5;                        // a scratch register: not context
    c << Type0(0x4000, 1) << 7;                        // an ALU constant: not context
    c << kType2 << 0 << kType2;                        // padding
    c << Type3(la::pm4::kSetConstant, 3) << ((4u << 16) | 0x10) << 21 << 22;  // 0x2010, 0x2011
    c << Type3(la::pm4::kSetConstant, 2) << ((0u << 16) | 0x10) << 99;        // ALU: ignored
    c << Type3(la::pm4::kSetConstant2, 2) << 0x2200 << 31;
    CHECK(WalkAll(hooks, c) == la::End::kQueueEnd);
    CHECK(hooks.registers.size() == 6);
    CHECK(hooks.registers[0x2000] == 11 && hooks.registers[0x2001] == 12);
    CHECK(hooks.registers[0x2100] == 3);
    CHECK(hooks.registers[0x2010] == 21 && hooks.registers[0x2011] == 22);
    CHECK(hooks.registers[0x2200] == 31);
  }

  // Draws: DRAW_INDX with its viz token and an index buffer, DRAW_INDX_2 auto-indexed; the
  // registers written before each are seen at it.
  {
    Hooks hooks;
    Words c;
    c << Type0(0x2208, 1) << 4;  // RB_MODECONTROL
    uint32_t dma_initiator = (100u << 16) | 4;           // 100 indices, triangle list, DMA
    c << Type3(la::pm4::kDrawIndx, 4) << 0 << dma_initiator << 0x1000 << 100;
    c << Type0(0x2208, 1) << 5;
    uint32_t auto_initiator = (3u << 16) | (2u << 6) | 4;  // auto-indexed
    c << Type3(la::pm4::kDrawIndx2, 1) << auto_initiator;
    CHECK(WalkAll(hooks, c) == la::End::kQueueEnd);
    CHECK(hooks.draws.size() == 2);
    CHECK(hooks.draws[0].initiator == dma_initiator && hooks.draws[0].dma);
    CHECK(hooks.draws[0].base == 0x1000 && hooks.draws[0].size == 100);
    CHECK(hooks.draws[0].registers[0x2208] == 4);
    CHECK(hooks.draws[1].initiator == auto_initiator && !hooks.draws[1].dma);
    CHECK(hooks.draws[1].registers[0x2208] == 5);
  }

  // Shaders: IM_LOAD from guest memory, IM_LOAD_IMMEDIATE from the packet, both as stored.
  {
    Hooks hooks;
    Words ucode;
    ucode << 0xAAAA0001 << 0xAAAA0002 << 0xAAAA0003;
    hooks.memory[0x00100000] = &ucode;
    Words c;
    c << Type3(la::pm4::kImLoad, 2) << (0x00100000u | 0) << 3;  // vertex
    c << Type3(la::pm4::kImLoadImmediate, 4) << 1 << 2 << 0xBBBB0001 << 0xBBBB0002;  // pixel
    c << Type3(la::pm4::kImLoad, 2) << (0x00200000u | 1) << 3;  // outside memory: skipped
    CHECK(WalkAll(hooks, c) == la::End::kQueueEnd);
    CHECK(hooks.shaders.size() == 2);
    CHECK(hooks.shaders[0].first == 0 && hooks.shaders[0].second == ucode.w);
    CHECK(hooks.shaders[1].first == 1 && hooks.shaders[1].second.size() == 2);
    CHECK(hooks.shaders[1].second[1] == la::Swap32(0xBBBB0002));
  }

  // Indirect buffers: walked where they are called, in order, nested up to the limit.
  {
    Hooks hooks;
    Words inner, outer, c;
    inner << Type0(0x2001, 1) << 2 << Type3(la::pm4::kDrawIndx2, 1) << ((2u << 6) | 4);
    hooks.memory[0x00300000] = &inner;
    outer << Type0(0x2001, 1) << 1 << Type3(la::pm4::kIndirectBuffer, 2) << 0x00300000
          << inner.size();
    hooks.memory[0x00400000] = &outer;
    c << Type3(la::pm4::kIndirectBufferPfd, 2) << (0x00400000u | 0x40000000u) << outer.size();
    c << Type0(0x2001, 1) << 3 << Type3(la::pm4::kDrawIndx2, 1) << ((2u << 6) | 4);
    CHECK(WalkAll(hooks, c) == la::End::kQueueEnd);
    CHECK(hooks.draws.size() == 2);
    CHECK(hooks.draws[0].registers[0x2001] == 2);
    CHECK(hooks.draws[1].registers[0x2001] == 3);
    // A buffer that calls itself stops at the depth limit.
    Hooks loop_hooks;
    Words self;
    self << Type3(la::pm4::kDrawIndx2, 1) << ((2u << 6) | 4)
         << Type3(la::pm4::kIndirectBuffer, 2) << 0x00500000 << 5;
    loop_hooks.memory[0x00500000] = &self;
    Words call;
    call << Type3(la::pm4::kIndirectBuffer, 2) << 0x00500000 << 5;
    CHECK(WalkAll(loop_hooks, call) == la::End::kQueueEnd);
    CHECK(loop_hooks.draws.size() == la::kMaxDepth);
  }

  // Predication: bin masks set in the stream; a predicated draw runs only for selected bins,
  // a predicated swap never.
  {
    Hooks hooks;
    Words c;
    c << Type3(la::pm4::kSetBinMask, 2) << 0 << 0x1;
    c << Type3(la::pm4::kSetBinSelect, 2) << 0 << 0x2;
    c << Type3(la::pm4::kDrawIndx2, 1, true) << ((2u << 6) | 4);  // skipped
    c << Type3(la::pm4::kSetBinSelectLo, 1) << 0x1;
    c << Type3(la::pm4::kDrawIndx2, 1, true) << ((2u << 6) | 5);  // drawn
    c << Type3(la::pm4::kXeSwap, 1, true) << 0;                   // never
    c << Type3(la::pm4::kXeSwap, 1) << 0;
    CHECK(WalkAll(hooks, c) == la::End::kQueueEnd);
    CHECK(hooks.draws.size() == 1 && (hooks.draws[0].initiator & 0x3F) == 5);
    CHECK(hooks.swaps == 1);
  }

  // Limits: packets; a draw that stops the walk; where the walk stopped.
  {
    Hooks hooks;
    Words c;
    for (int i = 0; i < 10; ++i) c << Type0(0x2001, 1) << uint32_t(i);
    la::Walker<Hooks>* walker;
    CHECK(WalkAll(hooks, c, 0, UINT32_MAX, 0, 4, &walker) == la::End::kPackets);
    CHECK(hooks.registers[0x2001] == 3 && walker->stopped() == 8);

    Hooks stop_hooks;
    stop_hooks.draw_result = la::End::kPipelines;
    stop_hooks.draw_result_at = 1;
    Words d;
    d << kType2;
    for (int i = 0; i < 4; ++i) d << Type3(la::pm4::kDrawIndx2, 1) << ((2u << 6) | 4);
    CHECK(WalkAll(stop_hooks, d, 0, UINT32_MAX, 0, 1000, &walker) == la::End::kPipelines);
    CHECK(stop_hooks.draws.size() == 2 && walker->stopped() == 3);
  }

  // The time limit is asked on every kind of packet: long runs of register writes (type 0 and
  // type 1) and of padding stop when it is reached, and padding counts towards the packet limit.
  {
    const uint32_t interval = la::kOutInterval;
    Words type0, type1, padding;
    for (uint32_t i = 0; i < 10000; ++i) {
      type0 << Type0(0x2001, 1) << i;
      type1 << (0x40000000u | 0x10u | (0x11u << 11)) << i << i;
      padding << ((i & 1) ? kType2 : 0u);
    }
    la::Walker<Hooks>* walker;
    Hooks h0;
    h0.out_after = 0;  // out from the start
    CHECK(WalkAll(h0, type0, 0, UINT32_MAX, 0, 1000000, &walker) == la::End::kTime);
    CHECK(h0.out_calls == 1 && walker->stopped() == (interval - 1) * 2);
    CHECK(h0.registers[0x2001] == interval - 2);
    Hooks h1;
    h1.out_after = 0;
    CHECK(WalkAll(h1, type1, 0, UINT32_MAX, 0, 1000000, &walker) == la::End::kTime);
    CHECK(h1.out_calls == 1 && walker->stopped() == (interval - 1) * 3);
    Hooks hp;
    hp.out_after = 2;  // out at the third question
    CHECK(WalkAll(hp, padding, 0, UINT32_MAX, 0, 1000000, &walker) == la::End::kTime);
    CHECK(hp.out_calls == 3 && walker->stopped() == 3 * interval - 1);
    Hooks hl;  // never out: the packet limit, padding words counted
    CHECK(WalkAll(hl, padding, 0, UINT32_MAX, 0, 1000, &walker) == la::End::kPackets);
    CHECK(walker->stopped() == 1000 && hl.out_calls == 1000 / interval);
    Hooks hall;  // and without limits the whole run is walked, asked all along
    CHECK(WalkAll(hall, type0, 0, UINT32_MAX, 0, 1000000, &walker) == la::End::kQueueEnd);
    CHECK(hall.registers[0x2001] == 9999 && hall.out_calls == 10000 / interval);
  }

  // A packet longer than what is queued; one longer than its indirect buffer ends only that
  // buffer.
  {
    Hooks hooks;
    Words c;
    c << Type0(0x2001, 1) << 1 << Type0(0x2002, 4) << 1 << 2;
    CHECK(WalkAll(hooks, c) == la::End::kBadPacket);
    CHECK(hooks.registers[0x2001] == 1 && !hooks.registers.count(0x2002));
    Hooks ib_hooks;
    Words bad;
    bad << Type0(0x2003, 3) << 1;
    ib_hooks.memory[0x00600000] = &bad;
    Words call;
    call << Type3(la::pm4::kIndirectBuffer, 2) << 0x00600000 << bad.size()
         << Type0(0x2004, 1) << 4;
    CHECK(WalkAll(ib_hooks, call) == la::End::kQueueEnd);
    CHECK(ib_hooks.registers[0x2004] == 4 && !ib_hooks.registers.count(0x2003));
  }

  // WAIT_REG_MEM on guest memory: an unmet wait stops the walk (the game may still be writing
  // what follows), a met one does not; a register wait never does.
  {
    Words flag;
    flag << 5;
    // Function 3 (equal), memory, the value with the 8-in-32 swap (2): as stored big-endian.
    auto wait = [](Words& c, uint32_t ref) {
      c << Type3(la::pm4::kWaitRegMem, 5) << (0x10 | 3) << (0x00700000u | 2) << ref
        << 0xFFFFFFFF << 0x100;
    };
    Hooks met;
    met.memory[0x00700000] = &flag;
    Words c1;
    wait(c1, 5);
    c1 << Type3(la::pm4::kDrawIndx2, 1) << ((2u << 6) | 4);
    CHECK(WalkAll(met, c1) == la::End::kQueueEnd && met.draws.size() == 1);
    Hooks unmet;
    unmet.memory[0x00700000] = &flag;
    Words c2;
    wait(c2, 6);
    c2 << Type3(la::pm4::kDrawIndx2, 1) << ((2u << 6) | 4);
    CHECK(WalkAll(unmet, c2) == la::End::kWait && unmet.draws.empty());
    // Told to go on past such waits, the walk only counts them.
    Hooks past;
    past.memory[0x00700000] = &flag;
    la::Walker<Hooks> past_walker(past, 1000, 0xFFFFFFFFull, 0xFFFFFFFFull);
    past_walker.set_stop_at_unmet_waits(false);
    CHECK(past_walker.Walk(c2.w.data(), 0, c2.size(), 0) == la::End::kQueueEnd);
    CHECK(past.draws.size() == 1 && past_walker.unmet_waits() == 1);
    Hooks reg;
    Words c3;
    c3 << Type3(la::pm4::kWaitRegMem, 5) << 3 << 0x0578 << 1 << 0xFFFFFFFF << 0x100;
    c3 << Type3(la::pm4::kDrawIndx2, 1) << ((2u << 6) | 4);
    CHECK(WalkAll(reg, c3) == la::End::kQueueEnd && reg.draws.size() == 1);
  }

  // The ring: a walk from near its end wraps to its start, packets and immediate microcode
  // across the end included.
  {
    Words ring;
    for (int i = 0; i < 16; ++i) ring << 0;
    // At 13: IM_LOAD_IMMEDIATE (header 13, type 14, size 15, words 0-1), then a draw at 2-3.
    auto at = [&](uint32_t i, uint32_t v) { ring.w[i % 16] = la::Swap32(v); };
    at(13, Type3(la::pm4::kImLoadImmediate, 4));
    at(14, 0);
    at(15, 2);
    at(16, 0xC0DE0001);
    at(17, 0xC0DE0002);
    at(18, Type3(la::pm4::kDrawIndx2, 1));
    at(19, (2u << 6) | 4);
    Hooks hooks;
    CHECK(WalkAll(hooks, ring, 13, 7, 16) == la::End::kQueueEnd);
    CHECK(hooks.shaders.size() == 1 && hooks.shaders[0].second[1] == la::Swap32(0xC0DE0002));
    CHECK(hooks.draws.size() == 1);
    CHECK(la::RingDistance(13, 4, 16) == 7 && la::RingDistance(4, 4, 16) == 0);
    CHECK(la::RingDistance(3, 9, 16) == 6);
  }

  std::printf("pipeline lookahead: all checks passed\n");
  return 0;
}
