#version 450
#extension GL_GOOGLE_include_directive : require
#include "vertex_lit.glsl"
layout(location=0) centroid in vec4 vertex_irradiance;
layout(location=0) invariant out vec4 color;
layout(set=0,binding=0,std140) uniform Material {
  vec4 fog;
  vec4 material;
  vec4 modulation;
  vec4 output_color;
  uvec4 output_rules;
} p;
void main() {
  vec4 base=vertex_lit_color(vertex_irradiance.rgb,p.fog,p.material.x,p.modulation.rgb);
  if(!vertex_lit_alpha_pass(base.a,p.output_color.y,p.output_rules.x))discard;
  gl_SampleMask[0]=int(vertex_lit_coverage(base.a,uvec2(gl_FragCoord.xy),p.output_rules.y,p.output_rules.z));
  precise vec4 exposed=base*p.output_color.x;
  if(p.output_rules.w!=0)exposed.rgb=vec3(vertex_lit_pwl_gamma(exposed.r),
      vertex_lit_pwl_gamma(exposed.g),vertex_lit_pwl_gamma(exposed.b));
  color=exposed;
}
