#version 450

layout(set=0, binding=0, std430) readonly buffer Vertices { uint words[]; } vertices;
layout(push_constant, std430) uniform Geometry {
  vec4 clip_columns[4];
  vec4 origin_scale;
  vec4 ndc_scale;
  vec4 ndc_offset;
  uvec4 layout_info;
} p;

uint endian_word(uint word, uint endian) {
  if (((endian ^ (endian >> 1)) & 1) != 0)
    word = ((word & 0x00ff00ffu) << 8) | ((word & 0xff00ff00u) >> 8);
  if ((endian & 2) != 0) word = (word << 16) | (word >> 16);
  return word;
}

void main() {
  uint index = endian_word(uint(gl_VertexIndex), p.layout_info.w);
  if (index >= p.layout_info.z) { gl_Position = vec4(0,0,-1,1); return; }
  uint address = index * p.layout_info.x;
  uvec2 packed_position = uvec2(endian_word(vertices.words[address], p.layout_info.y),
                       endian_word(vertices.words[address+1], p.layout_info.y));
  ivec4 signed_position = ivec4(int(packed_position.x) >> 16, int(packed_position.x << 16) >> 16,
                               int(packed_position.y) >> 16, int(packed_position.y << 16) >> 16);
  precise vec4 q = max(vec4(signed_position) * (1.0 / 32767.0), vec4(-1));
  precise float scale = q.w * p.origin_scale.w;
  precise vec3 world = q.xyz * scale + p.origin_scale.xyz;
  precise vec4 clip = world.z * p.clip_columns[2] + p.clip_columns[3];
  clip = world.y * p.clip_columns[1] + clip;
  clip = world.x * p.clip_columns[0] + clip;
  precise vec3 ndc = clip.xyz * p.ndc_scale.xyz + p.ndc_offset.xyz * clip.w;
  gl_Position = vec4(ndc, clip.w);
}
