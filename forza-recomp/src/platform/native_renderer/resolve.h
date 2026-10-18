// The tiled address mapping is derived from ReXGlue/Xenia under the BSD
// license reproduced in resolve-LICENSE.txt. Other code uses the project license.
#pragma once

#include <array>
#include <cstdint>

namespace forza::native_renderer {

// A native RGBA8 rectangle resolved directly to the guest's tiled/scaled
// 32-bit quad layout. All coordinates except destination.xy and pitch are host
// pixels. The caller owns both resources and their synchronization/lifetimes.
struct alignas(16) ResolveParameters {
  std::array<uint32_t, 4> source{};       // x, y, width, height
  std::array<uint32_t, 4> destination{};  // guest x, guest y, guest pitch, byte base
  std::array<uint32_t, 4> layout{1, 1, 0, 0}; // scale x/y, Endian128, swap R/B
  std::array<uint32_t, 4> bounds{};       // buffer bytes, image width/height, reserved
};
static_assert(sizeof(ResolveParameters) == 64);

// The 32-bit tiled address contract used by ReXGlue/Xenia. Kept separate from
// resource identity: a guest address never grants ownership of a native image.
inline uint32_t Tiled32Offset(uint32_t x, uint32_t y, uint32_t pitch) {
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 9;
  const uint32_t micro = ((x & 7) + ((y & 14) << 2)) << 2;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
  return ((offset & ~511u) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) +
      (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}

inline uint64_t ResolveBufferBytes(const ResolveParameters& p) {
  if (!p.layout[0] || !p.layout[1]) return 0;
  const uint64_t end_y = uint64_t(p.destination[1]) + p.source[3] / p.layout[1];
  return uint64_t(p.destination[3]) + ((end_y + 31) & ~uint64_t(31)) *
      p.destination[2] * 4 * p.layout[0] * p.layout[1];
}

inline bool Valid(const ResolveParameters& p) {
  const auto sx = p.layout[0], sy = p.layout[1];
  if (!sx || sx > 7 || !sy || sy > 7 || p.layout[2] > 5 || p.layout[3] > 1 || p.bounds[3] ||
      !p.source[2] || !p.source[3] || p.source[2] % (4 * sx) || p.source[3] % sy ||
      !p.destination[2] || p.destination[2] > 16384 || p.destination[2] % 32 ||
      p.destination[0] % 8 || p.destination[1] % 8 || p.destination[3] % 16 ||
      p.bounds[0] % 16 || p.bounds[1] > 32768 || p.bounds[2] > 32768)
    return false;
  if (uint64_t(p.source[0]) + p.source[2] > p.bounds[1] ||
      uint64_t(p.source[1]) + p.source[3] > p.bounds[2] ||
      uint64_t(p.destination[0]) + p.source[2] / sx > p.destination[2] ||
      uint64_t(p.destination[1]) + p.source[3] / sy > 16384)
    return false;
  return ResolveBufferBytes(p) <= p.bounds[0];
}

// One invocation writes four consecutive host pixels as a complete 16-byte
// destination quad. This avoids read-modify-write races between scaled pixels.
inline uint64_t ResolveQuadOffset(const ResolveParameters& p, uint32_t host_quad_x, uint32_t host_y) {
  const uint32_t sx = p.layout[0], sy = p.layout[1];
  const uint32_t x = p.destination[0] + (host_quad_x / sx) * 4;
  const uint32_t y = p.destination[1] + host_y / sy;
  return p.destination[3] + uint64_t(Tiled32Offset(x, y, p.destination[2])) * sx * sy +
      ((host_quad_x % sx) * sy + host_y % sy) * 16;
}

inline uint32_t SwapResolveWord(uint32_t word, uint32_t endian) {
  if (endian >= 4) endian = 2;
  if (((endian ^ (endian >> 1)) & 1) != 0)
    word = ((word & 0x00ff00ffu) << 8) | ((word & 0xff00ff00u) >> 8);
  if (endian & 2) word = (word << 16) | (word >> 16);
  return word;
}

inline std::array<uint32_t, 4> EncodeResolveQuad(std::array<std::array<uint8_t, 4>, 4> rgba,
                                               uint32_t endian, bool swap_rb) {
  std::array<uint32_t, 4> result{};
  for (uint32_t i = 0; i < 4; ++i) {
    const auto& c = rgba[i];
    const uint32_t word = uint32_t(c[swap_rb ? 2 : 0]) | (uint32_t(c[1]) << 8) |
        (uint32_t(c[swap_rb ? 0 : 2]) << 16) | (uint32_t(c[3]) << 24);
    const uint32_t lane = endian == 4 ? i ^ 1 : (endian == 5 ? 3 - i : i);
    result[lane] = SwapResolveWord(word, endian);
  }
  return result;
}

} // namespace forza::native_renderer
