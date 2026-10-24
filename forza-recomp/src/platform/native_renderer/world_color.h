#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace forza::native_renderer {
// World-color finishing: exposure, edge-aware scene filtering, bloom, grading,
// vignette and the output color matrix. The UI is composed downstream.
// Parameters are immutable for one region; different regions may use different
// exposure/tint controls even when they read the same scene image.
struct alignas(16) WorldColorParameters {
  std::array<std::array<float,4>,16> color{};
  std::array<std::array<float,4>,11> view{};
  std::array<float,4> ndc_scale{1,1,1,0};
  std::array<float,4> ndc_offset{};
  std::array<float,4> scene_noise_bias{};
  std::array<float,4> bloom_lut_bias{};
  std::array<float,4> input_scale{1,1,1,1}; // scene, noise, bloom, grading
  std::array<float,4> exposure_bias_scale{};
  std::array<uint32_t,4> rules{}; // vertex endian, PWL gamma, reserved, reserved
};
static_assert(sizeof(WorldColorParameters)==544);
inline bool Valid(const WorldColorParameters& p) {
  for(const auto& v:p.color)for(float x:v)if(!std::isfinite(x))return false;
  for(const auto& v:p.view)for(float x:v)if(!std::isfinite(x))return false;
  for(const auto* v:{&p.ndc_scale,&p.ndc_offset,&p.scene_noise_bias,&p.bloom_lut_bias,
                     &p.input_scale,&p.exposure_bias_scale})
    for(float x:*v)if(!std::isfinite(x))return false;
  const float base=p.view[5][1]*p.view[5][0];
  const float denominator=(base+p.view[5][2])*p.view[5][0]+p.view[7][1];
  const float numerator=(base+p.view[7][0])*p.view[5][0]+p.view[7][2];
  if(denominator==0)return false;
  const float white=numerator/denominator-p.view[7][3];
  if(!std::isfinite(white) || white<=0 || p.exposure_bias_scale[2]<=0 ||
      p.color[11][1]<0 || p.color[11][2]<0 || p.color[12][1]<=0 || p.color[15][3]<0)return false;
  for(float x:p.input_scale)if(x<0)return false;
  return p.rules[0]<=3 && p.rules[1]<=1 && !p.rules[2] && !p.rules[3] &&
      p.view[2][1]>p.view[2][0] && p.view[2][2]>p.view[2][1] &&
      p.view[2][3]>p.view[2][2];
}
} // namespace forza::native_renderer
