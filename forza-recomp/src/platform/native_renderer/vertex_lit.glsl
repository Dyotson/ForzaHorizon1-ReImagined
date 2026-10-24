// Authored vertex-lit material and output operations. The material is independent
// of guest registers, descriptor layouts and attachment allocation.
float vertex_lit_multiply(float a,float b) {return a==0 || b==0?0:a*b;}
vec4 vertex_lit_color(vec3 irradiance,vec4 fog,float opacity,vec3 modulation) {
  precise vec3 lit=vec3(vertex_lit_multiply(irradiance.x,modulation.x),
      vertex_lit_multiply(irradiance.y,modulation.y),vertex_lit_multiply(irradiance.z,modulation.z));
  precise vec3 difference=fog.rgb-lit;
  precise vec3 mixed=vec3(vertex_lit_multiply(difference.x,fog.a),
      vertex_lit_multiply(difference.y,fog.a),vertex_lit_multiply(difference.z,fog.a))+lit;
  return vec4(sqrt(abs(mixed)),opacity);
}
bool vertex_lit_alpha_pass(float alpha,float reference,uint comparison) {
  if(comparison==7)return true;
  if(comparison==5)return alpha!=reference;
  return ((comparison&1)!=0 && alpha<reference) || ((comparison&2)!=0 && alpha==reference) ||
      ((comparison&4)!=0 && alpha>reference);
}
uint vertex_lit_coverage(float alpha,uvec2 pixel,uint samples,uint pattern) {
  if(pattern==0)return 0xffffffffu;
  float offset=float((pattern>>(((pixel.x&1)|((pixel.y&1)<<1))*2))&3);
  if(samples==4)return (alpha>=0.75-offset*0.0625?1u:0u) |
      (alpha>=0.5-offset*0.0625?2u:0u) | (alpha>=0.25-offset*0.0625?4u:0u) |
      (alpha>=1-offset*0.0625?8u:0u);
  if(samples==2)return (alpha>=1-offset*0.125?1u:0u) | (alpha>=0.5-offset*0.125?2u:0u);
  return alpha>=1-offset*0.25?1u:0u;
}
float vertex_lit_pwl_gamma(float value) {
  value=isnan(value)?0:clamp(value,0,1);
  float slope=1023,offset=0;
  if(value>=512.0/1023){slope=127.875;offset=128.0/255;}
  else if(value>=128.0/1023){slope=255.75;offset=64.0/255;}
  else if(value>=64.0/1023){slope=511.5;offset=32.0/255;}
  precise float encoded=trunc(value*slope)*(1.0/255)+offset;
  return encoded;
}
