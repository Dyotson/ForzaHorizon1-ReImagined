#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace forza::native_renderer {

using GeometryVector = std::array<float, 4>;

// A native rigid-mesh contract. Positions are four signed normalized 16-bit
// components, stored as two words with the first component in the upper half.
// XYZ are multiplied by W * origin_scale.w, then translated by origin_scale.xyz.
// Matrices are columns. The explicit arithmetic order avoids fused operations
// changing depth along shared edges. No guest register layout is exposed here.
struct alignas(16) GeometryParameters {
  std::array<GeometryVector, 4> clip_columns{{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}}};
  GeometryVector origin_scale{0,0,0,1};
  GeometryVector ndc_scale{1,1,1,0};
  GeometryVector ndc_offset{};
  std::array<uint32_t, 4> layout{2,0,0,0}; // stride in words, vertex endian, count, index endian
};
static_assert(sizeof(GeometryParameters) == 128);

inline bool Valid(const GeometryParameters& p) {
  if (p.layout[0] < 2 || p.layout[0] > 64 || p.layout[1] > 3 ||
      !p.layout[2] || p.layout[2] > 0x1000000 || p.layout[3] > 3 ||
      p.ndc_scale[3] != 0 || p.ndc_offset[3] != 0) return false;
  for (const auto& c : p.clip_columns) for (float v : c) if (!std::isfinite(v)) return false;
  for (const auto* c : {&p.origin_scale, &p.ndc_scale, &p.ndc_offset})
    for (float v : *c) if (!std::isfinite(v)) return false;
  // Bound every intermediate, including the post-projection offset, so finite
  // inputs cannot overflow into undefined raster coordinates.
  std::array<double,4> clip_bound{};
  for (uint32_t i=0;i<4;++i) {
    clip_bound[i]=std::abs(double(p.clip_columns[3][i]));
    for (uint32_t j=0;j<3;++j)
      clip_bound[i]+=(std::abs(double(p.origin_scale[j]))+std::abs(double(p.origin_scale[3])))*
          std::abs(double(p.clip_columns[j][i]));
    if (clip_bound[i]>std::numeric_limits<float>::max()) return false;
  }
  for (uint32_t i=0;i<3;++i)
    if (clip_bound[i]*std::abs(double(p.ndc_scale[i]))+
        clip_bound[3]*std::abs(double(p.ndc_offset[i]))>std::numeric_limits<float>::max()) return false;
  return true;
}

inline uint32_t GeometryEndian(uint32_t word, uint32_t endian) {
  if (((endian ^ (endian >> 1)) & 1) != 0)
    word = ((word & 0x00ff00ffu) << 8) | ((word & 0xff00ff00u) >> 8);
  if (endian & 2) word = (word << 16) | (word >> 16);
  return word;
}

inline float GeometrySnorm(uint32_t value) {
  const int32_t signed_value = (value & 0x8000) ? int32_t(value & 0xffff) - 65536 : int32_t(value & 0xffff);
  const float normalized = float(signed_value) * (1.0f / 32767.0f);
  return normalized < -1.0f ? -1.0f : normalized;
}

inline GeometryVector GeometryPosition(const GeometryParameters& p, std::array<uint32_t, 2> words) {
  for (auto& word : words) word = GeometryEndian(word, p.layout[1]);
  const GeometryVector q{GeometrySnorm(words[0] >> 16), GeometrySnorm(words[0]),
                         GeometrySnorm(words[1] >> 16), GeometrySnorm(words[1])};
  const float scale = q[3] * p.origin_scale[3];
  GeometryVector world{};
  for (uint32_t i = 0; i < 3; ++i) world[i] = q[i] * scale + p.origin_scale[i];
  GeometryVector clip{};
  for (uint32_t i = 0; i < 4; ++i) {
    float product = world[2] * p.clip_columns[2][i];
    float sum = product + p.clip_columns[3][i];
    product = world[1] * p.clip_columns[1][i]; sum = product + sum;
    product = world[0] * p.clip_columns[0][i]; clip[i] = product + sum;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    const float scaled = clip[i] * p.ndc_scale[i];
    const float offset = p.ndc_offset[i] * clip[3];
    clip[i] = scaled + offset;
  }
  return clip;
}

}  // namespace forza::native_renderer
