#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace forza::native_renderer {
// A vertex-lit material consumes interpolated linear irradiance. Modulation
// precedes fog; the square-root encoding and exposure follow it. Attachment
// format, blending and depth/stencil remain the caller's responsibility.
struct alignas(16) VertexLitParameters {
  std::array<float,4> fog{};
  std::array<float,4> material{1,0,0,0}; // opacity, reserved
  std::array<float,4> modulation{1,1,1,0};
  std::array<float,4> output_color{1,0,0,0}; // exposure, alpha reference, reserved
  std::array<uint32_t,4> output_rules{7,1,0,0}; // comparison, samples, coverage pattern, PWL gamma
};
static_assert(sizeof(VertexLitParameters)==80);
static_assert(alignof(VertexLitParameters)==16);
static_assert(offsetof(VertexLitParameters,material)==16);
static_assert(offsetof(VertexLitParameters,modulation)==32);
static_assert(offsetof(VertexLitParameters,output_color)==48);
static_assert(offsetof(VertexLitParameters,output_rules)==64);

inline bool Valid(const VertexLitParameters& p) {
  for(const auto* v:{&p.fog,&p.material,&p.modulation,&p.output_color})
    for(float x:*v)if(!std::isfinite(x))return false;
  return p.material[1]==0 && p.material[2]==0 && p.material[3]==0 && p.modulation[3]==0 &&
      p.output_color[0]>=0 && p.output_color[2]==0 && p.output_color[3]==0 &&
      p.output_rules[0]<=7 && (p.output_rules[1]==1 || p.output_rules[1]==2 || p.output_rules[1]==4) &&
      p.output_rules[2]<=511 && p.output_rules[3]<=1;
}
inline float VertexLitMultiply(float a,float b) {return a==0 || b==0?0:a*b;}
inline bool VertexLitAlphaPass(float alpha,float reference,uint32_t comparison) {
  if(comparison==7)return true;
  if(comparison==5)return alpha!=reference;
  return ((comparison&1) && alpha<reference) || ((comparison&2) && alpha==reference) ||
      ((comparison&4) && alpha>reference);
}
inline uint32_t VertexLitCoverage(float alpha,uint32_t x,uint32_t y,uint32_t samples,uint32_t pattern) {
  if(!pattern)return UINT32_MAX;
  const float offset=float((pattern>>(((x&1)|((y&1)<<1))*2))&3);
  if(samples==4)return (alpha>=0.75f-offset*0.0625f?1u:0u) |
      (alpha>=0.5f-offset*0.0625f?2u:0u) | (alpha>=0.25f-offset*0.0625f?4u:0u) |
      (alpha>=1-offset*0.0625f?8u:0u);
  if(samples==2)return (alpha>=1-offset*0.125f?1u:0u) | (alpha>=0.5f-offset*0.125f?2u:0u);
  return alpha>=1-offset*0.25f?1u:0u;
}
// The existing render contract's piecewise-linear 8-bit gamma mapping. This is
// deliberately separate from the material's square-root encoding.
inline float VertexLitPwlGamma(float value) {
  value=std::isnan(value)?0:std::fmax(0,std::fmin(1,value));
  float slope=1023,offset=0;
  if(value>=512.0f/1023){slope=127.875f;offset=128.0f/255;}
  else if(value>=128.0f/1023){slope=255.75f;offset=64.0f/255;}
  else if(value>=64.0f/1023){slope=511.5f;offset=32.0f/255;}
  return std::trunc(value*slope)*(1.0f/255)+offset;
}
inline std::array<float,4> ShadeVertexLit(const std::array<float,3>& irradiance,const VertexLitParameters& p) {
  std::array<float,4> result{};
  for(uint32_t i=0;i<3;++i) {
    const float lit=VertexLitMultiply(irradiance[i],p.modulation[i]);
    const float difference=p.fog[i]-lit;
    const float mixed=VertexLitMultiply(difference,p.fog[3])+lit;
    result[i]=std::sqrt(std::abs(mixed))*p.output_color[0];
    if(p.output_rules[3])result[i]=VertexLitPwlGamma(result[i]);
  }
  result[3]=p.material[0]*p.output_color[0];
  return result;
}
} // namespace forza::native_renderer
