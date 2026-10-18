#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace forza::native_renderer {

using Float4 = std::array<float, 4>;

// A host contract, not a copy of the guest register file. Matrices transform a
// reconstructed position into the preceding frame's clip coordinates.
struct alignas(16) VelocityParameters {
  std::array<Float4, 100> previous{};
  Float4 projection{};  // depth denominator offset, depth numerator, reserved
  Float4 blur{4.0f, 16.666667f, 1.0f, 0.1f};  // power, scale, boost, limit
  Float4 uv_transform{1.0f, 1.0f, 0.0f, 0.0f};
  Float4 motion_scale{0.5f, -0.5f, 0.0f, 0.0f};
};
static_assert(sizeof(VelocityParameters) == 104 * 16);

struct VelocitySample {
  float depth = 0;
  std::array<float, 2> pixels{};
  Float4 blur{};
};

inline float DecodeDepth(const std::array<uint8_t, 4>& packed) {
  // The sampled RGBA bytes contain the middle/low depth bytes, stencil and
  // exponent/high mantissa byte respectively. Depth uses a 20-bit mantissa.
  const uint32_t mantissa = (uint32_t(packed[3] & 15) << 16) |
                            (uint32_t(packed[0]) << 8) | packed[1];
  return std::ldexp(1.0f + float(mantissa) / 1048576.0f,
                    int(packed[3] >> 4) - 15);
}

inline bool Valid(const VelocityParameters& parameters) {
  for (const auto& row : parameters.previous)
    for (float value : row)
      if (!std::isfinite(value)) return false;
  for (const auto* row : {&parameters.projection, &parameters.blur,
                          &parameters.uv_transform, &parameters.motion_scale})
    for (float value : *row)
      if (!std::isfinite(value)) return false;
  for (uint32_t axis = 0; axis < 2; ++axis)
    if (!((parameters.uv_transform[axis] == 1 && parameters.uv_transform[axis + 2] == 0) ||
          (parameters.uv_transform[axis] == -1 && parameters.uv_transform[axis + 2] == 1)))
      return false;
  return parameters.blur[0] > 0 && parameters.blur[1] >= 0 &&
         parameters.blur[2] >= 0 && parameters.blur[3] > 0;
}

// Reference implementation for GPU validation. Coordinates identify the center
// of a pixel in the output; UV orientation is an explicit integration choice.
inline VelocitySample EvaluateVelocity(const VelocityParameters& parameters,
    const std::array<uint8_t, 4>& packed, float u, float v,
    uint32_t width, uint32_t height) {
  VelocitySample result;
  result.depth = std::clamp(DecodeDepth(packed), 0.0f, 1.0f);
  const uint32_t stencil = packed[2];
  const uint32_t object = stencil >= 128 ? 0 : std::min((stencil + 1) / 3, 24u);
  const float x = (u * parameters.uv_transform[0] + parameters.uv_transform[2]) * 2 - 1;
  const float y = 1 - (v * parameters.uv_transform[1] + parameters.uv_transform[3]) * 2;
  const float denominator = 1 - parameters.projection[0] - result.depth;
  const float z = parameters.projection[1] /
      (std::abs(denominator) > 1e-20f ? denominator : -1e-20f);
  const Float4 position{x * z, y * z, z * (1 - result.depth), z};
  auto dot = [&](uint32_t row) {
    float value = 0;
    for (uint32_t c = 0; c < 4; ++c)
      value += position[c] * parameters.previous[object * 4 + row][c];
    return value;
  };
  const float w = dot(3);
  float dx = dot(0) / w - x, dy = dot(1) / w - y;
  if (std::abs(w) < 1e-20f || !std::isfinite(dx) || !std::isfinite(dy))
    dx = dy = 0;
  result.pixels = {
      std::clamp(dx * width * parameters.motion_scale[0], -float(width), float(width)),
      std::clamp(dy * height * parameters.motion_scale[1], -float(height), float(height))};
  // Blur has a deliberately different contract: a shaped, limited displacement
  // and a separate speed channel. Temporal reconstruction uses unclamped motion.
  const float speed = std::hypot(dx, dy);
  const float shaped = speed + speed * parameters.blur[2] /
      (1 + std::pow(speed * parameters.blur[1], parameters.blur[0]));
  const float amount = speed > 1e-20f ? std::min(shaped, parameters.blur[3]) / speed : 0;
  result.blur = {std::clamp(-dx * amount * 5 + 127.0f / 255.0f, 0.0f, 1.0f),
                 std::clamp(dy * amount * 5 + 127.0f / 255.0f, 0.0f, 1.0f),
                 std::clamp(shaped, 0.0f, 1.0f), float(stencil > 0 && stencil < 128)};
  return result;
}

}  // namespace forza::native_renderer
