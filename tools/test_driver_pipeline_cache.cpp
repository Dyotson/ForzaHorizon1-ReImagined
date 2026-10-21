// Standalone corruption/replacement checks. No game files, GPU or Vulkan SDK required.
#include <rex/graphics/vulkan/driver_cache_file.h>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace rex::graphics::vulkan::driver_cache_file;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
int main() {
  const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
  auto root = temporary /
      ("fh1-driver-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  bool created = false;
  auto cleanup = [&] {
    if (created && std::filesystem::weakly_canonical(root).parent_path() == temporary)
      std::filesystem::remove_all(root);
  };
  try {
    Identity id{0x10de, 0x1234, 17, 0x300000004, {}};
    for (size_t i = 0; i < 16; ++i) id.uuid[i] = uint8_t(i * 13);
    std::vector<uint8_t> payload(128, 0xab);
    Put32(payload.data(), 32); Put32(payload.data() + 4, 1);
    Put32(payload.data() + 8, id.vendor); Put32(payload.data() + 12, id.device);
    std::copy(id.uuid.begin(), id.uuid.end(), payload.begin() + 16);
    const auto file = Encode(payload, id); CHECK(Decode(file, id) == payload);
    for (size_t i = 0; i < file.size(); ++i) {
      auto broken = file; broken[i] ^= 1; CHECK(Decode(broken, id).empty());
    }
    for (size_t n = 0; n < file.size(); ++n) {
      CHECK(Decode(std::vector<uint8_t>(file.begin(), file.begin() + n), id).empty());
    }
    auto appended = file; appended.push_back(0); CHECK(Decode(appended, id).empty());
    auto other = id; ++other.vendor; CHECK(Decode(file, other).empty());
    other = id; ++other.device; CHECK(Decode(file, other).empty());
    other = id; ++other.driver; CHECK(Decode(file, other).empty());
    other = id; ++other.abi; CHECK(Decode(file, other).empty());
    other = id; ++other.uuid[5]; CHECK(Decode(file, other).empty());
    auto invalid = payload; Put32(invalid.data(), 1000); CHECK(Encode(invalid, id).empty());
    invalid = payload; Put32(invalid.data() + 4, 2); CHECK(Encode(invalid, id).empty());
    CHECK(std::filesystem::create_directory(root)); created = true;
    const auto path = root / "cache.vkpc";
    CHECK(Read(path, id).empty()); CHECK(Write(path, payload, id)); CHECK(Read(path, id) == payload);
    auto replacement = payload; replacement.back() = 0xcd;
    CHECK(Write(path, replacement, id)); CHECK(Read(path, id) == replacement);
    CHECK(!Write(path, invalid, id)); CHECK(Read(path, id) == replacement);
    std::ofstream(root / "blocked").put('x');
    CHECK(!Write(root / "blocked" / "child", payload, id));
    {
      std::ofstream oversized(root / "oversized", std::ios::binary);
      oversized.seekp(kMaxPayload + kHeaderSize + 1); oversized.put(0);
    }
    CHECK(Read(root / "oversized", id).empty());
    std::atomic<bool> finish{false}, failed{false};
    std::thread reader([&] {
      while (!finish) {
        const auto current = Read(path, id);
        // Windows may deny a racing open during replacement. A missing cache
        // is a supported fallback; a nonempty partial or mixed result is not.
        if (!current.empty() && current != payload && current != replacement) failed = true;
      }
    });
    for (size_t i = 0; i < 50; ++i) Write(path, i % 2 ? payload : replacement, id);
    finish = true; reader.join(); CHECK(!failed);
    CHECK(Write(path, replacement, id)); CHECK(Read(path, id) == replacement);
    cleanup();
    std::cout << "Driver cache identity, corruption, size bounds and atomic replacement passed\n";
    return 0;
  } catch (const std::exception& e) {
    cleanup();
    std::cerr << e.what() << '\n'; return 1;
  }
}
