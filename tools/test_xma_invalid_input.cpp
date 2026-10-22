// The XMA decoder drops input it cannot read instead of crashing or spinning on it, and does
// not touch a context the game has locked (patch 0105, #228). Links the runtime; no game
// files needed. Every kick runs on a watchdog: unpatched, some cases spin forever, and the
// faults (#228's among them) never return either, as the runtime's fault handler retries them.
#include <rex/audio/xma/context.h>
#include <rex/memory.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
using rex::audio::XMA_CONTEXT_DATA;
using rex::audio::XmaContext;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

namespace {

constexpr uint32_t kPacket = XmaContext::kBytesPerPacket;

// Ends the test while a kick is stuck. The guest memory file in /dev/shm (several GB) is
// removed first: no destructor runs.
[[noreturn]] void Abandon(const char* why) {
  std::cerr << "xma invalid input: FAIL: " << why << "\n";
  std::ifstream maps("/proc/self/maps");
  for (std::string line; std::getline(maps, line);) {
    const auto at = line.find("/dev/shm/xenia_memory_");
    if (at == std::string::npos) continue;
    std::string path = line.substr(at);
    if (const auto deleted = path.find(" (deleted)"); deleted != std::string::npos) continue;
    std::remove(path.c_str());
  }
  std::_Exit(1);
}

struct Fixture {
  rex::memory::Memory memory;
  XmaContext context;
  uint32_t context_va = 0, input_va = 0, input_phys = 0, output_phys = 0;

  Fixture() {
    CHECK(memory.Initialize());
    const uint32_t physical = rex::memory::kSystemHeapPhysical;
    context_va = memory.SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA), 256, physical);
    input_va = memory.SystemHeapAlloc(4 * kPacket, kPacket, physical);
    const uint32_t output_va = memory.SystemHeapAlloc(8 * XmaContext::kOutputBytesPerBlock, 256,
                                                      physical);
    CHECK(context_va && input_va && output_va);
    input_phys = memory.GetPhysicalAddress(input_va);
    output_phys = memory.GetPhysicalAddress(output_va);
    CHECK(context.Setup(0, &memory, context_va) == 0);
    context.set_is_allocated(true);
  }

  uint8_t* input() { return memory.TranslateVirtual(input_va); }
  uint8_t* raw() { return memory.TranslateVirtual(context_va); }

  // Buffer 0 current and valid, output room for 8 blocks, nothing decoded yet.
  XMA_CONTEXT_DATA Base(uint32_t address, uint32_t packets) {
    const uint8_t zero[sizeof(XMA_CONTEXT_DATA)] = {};
    XMA_CONTEXT_DATA data(zero);
    data.input_buffer_0_ptr = address;
    data.input_buffer_0_packet_count = packets;
    data.input_buffer_0_valid = 1;
    data.input_buffer_read_offset = XmaContext::kBitsPerPacketHeader;
    data.output_buffer_ptr = output_phys;
    data.output_buffer_block_count = 8;
    data.output_buffer_valid = 1;
    data.sample_rate = 3;
    return data;
  }

  // One kick (the guest's XMAEnableContext) on a watchdog; the context afterwards.
  XMA_CONTEXT_DATA Kick(XMA_CONTEXT_DATA data) {
    data.Store(raw());
    context.Enable();
    auto work = std::async(std::launch::async, [this] { return context.Work(); });
    if (work.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
      Abandon("Work() did not return in 10 s");
    }
    CHECK(work.get());
    return XMA_CONTEXT_DATA(raw());
  }
};

// XMA2 packet header: frames starting in the packet, offset of the first one in bits after
// the 32-bit header, metadata 1 (XMA2), packets to skip.
void PacketHeader(uint8_t* packet, uint32_t frames, uint32_t first_frame_bits, uint8_t skip) {
  packet[0] = uint8_t(frames << 2 | (first_frame_bits >> 13 & 3));
  packet[1] = uint8_t(first_frame_bits >> 5);
  packet[2] = uint8_t((first_frame_bits & 0x1F) << 3 | 1);
  packet[3] = skip;
}

// Writes a big-endian bit field at `bit` (from the start of the packet).
void PutBits(uint8_t* packet, uint32_t bit, uint32_t value, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t b = bit + i;
    const uint8_t mask = uint8_t(0x80 >> (b & 7));
    if (value >> (count - 1 - i) & 1) {
      packet[b >> 3] |= mask;
    } else {
      packet[b >> 3] &= uint8_t(~mask);
    }
  }
}

void ExpectDropped(const XMA_CONTEXT_DATA& after, const char* what) {
  if (after.input_buffer_0_valid || after.output_buffer_write_offset != 0) {
    throw std::runtime_error(std::string(what) + ": buffer not dropped, or output written");
  }
}

// The guest locks a context (Disable, then Block) and rewrites it while the decoder thread
// keeps polling every context, as XmaDecoder::WorkerThreadMain does. Nothing may change the
// context while it is locked. Returns the number of times something did.
int LockedContextWrites(Fixture& f, int rounds) {
  std::atomic<bool> stop{false};
  std::thread worker([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      if (f.context.Work()) f.context.SignalWorkDone();
    }
  });
  uint8_t* packet = f.input();
  std::memset(packet, 0, kPacket);
  PacketHeader(packet, 0, 0, 0);
  XMA_CONTEXT_DATA setup = f.Base(f.input_phys, 1);
  uint8_t expected[sizeof(XMA_CONTEXT_DATA)];
  int writes = 0;
  setup.Store(f.raw());
  for (int i = 0; i < rounds; ++i) {
    f.context.Enable();  // kick, as XmaDecoder::WriteRegister does
    if (f.context.Work()) f.context.SignalWorkDone();
    f.context.Disable();  // lock
    f.context.Block(false);
    setup.Store(f.raw());  // the game sets the context up again
    std::memcpy(expected, f.raw(), sizeof(expected));
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
    while (std::chrono::steady_clock::now() < until) {
      if (std::memcmp(expected, f.raw(), sizeof(expected)) != 0) {
        ++writes;
        break;
      }
    }
  }
  stop = true;
  worker.join();
  return writes;
}

}  // namespace

int main() {
  int rc = 0;
  try {
    Fixture f;

    // Valid, but no address: dropped, nothing decoded.
    ExpectDropped(f.Kick(f.Base(0, 1)), "null address");

    // Valid, but no packets, while the other buffer has some (so the context is not a
    // consume-only one): the unpatched decoder returned from Decode() unchanged, forever.
    {
      XMA_CONTEXT_DATA data = f.Base(f.input_phys, 0);
      data.input_buffer_1_ptr = f.input_phys;
      data.input_buffer_1_packet_count = 1;
      std::memset(f.input(), 0, kPacket);
      const XMA_CONTEXT_DATA after = f.Kick(data);
      CHECK(!after.input_buffer_0_valid);
    }

    // Packets running past the end of guest physical memory (read past it unpatched).
    ExpectDropped(f.Kick(f.Base(0x1FFFF800, 4)), "past physical memory");

    // Read offset past the buffer's last packet (looped forever unpatched).
    {
      std::memset(f.input(), 0, kPacket);
      PacketHeader(f.input(), 1, 0, 0);
      XMA_CONTEXT_DATA data = f.Base(f.input_phys, 1);
      data.input_buffer_read_offset = 2 * XmaContext::kBitsPerPacket + 100;
      ExpectDropped(f.Kick(data), "read offset past the end");
    }

    // #228: frames shorter than their 15-bit header, starting one bit into a byte. Under 7
    // bits, BitStream::Copy took the 7 bits left in the byte, its count wrapped around, and
    // memcpy got about 2^61 bytes (a fault the kernel reports at address 0).
    for (uint32_t size : {1u, 3u, 6u, 14u}) {
      uint8_t* packet = f.input();
      std::memset(packet, 0, kPacket);
      PacketHeader(packet, 1, 1, 0);
      PutBits(packet, 33, size, XmaContext::kBitsPerFrameHeader);
      ExpectDropped(f.Kick(f.Base(f.input_phys, 1)), "frame shorter than its header");
    }

    // A frame longer than the two packets the decoder copies, in a buffer with a second
    // packet: skipped to that packet, which is played (here: has no frames, so the buffer ends).
    {
      uint8_t* packets = f.input();
      std::memset(packets, 0, 2 * kPacket);
      PacketHeader(packets, 1, 1, 0);
      PutBits(packets, 33, 0x7FFE, XmaContext::kBitsPerFrameHeader);
      PacketHeader(packets + kPacket, 0, 0x7FFF - 32, 0);
      ExpectDropped(f.Kick(f.Base(f.input_phys, 2)), "frame longer than two packets");
    }

    const int writes = LockedContextWrites(f, 20000);
    if (writes) {
      std::cerr << "xma invalid input: the decoder wrote a locked context " << writes
                << " times\n";
      rc = 1;
    }
    if (!rc) std::cout << "xma invalid input: ok\n";
  } catch (const std::exception& e) {
    std::cerr << "xma invalid input: FAIL: " << e.what() << "\n";
    rc = 1;
  }
  std::fflush(nullptr);
  return rc;
}
