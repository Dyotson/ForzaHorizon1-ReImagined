#version 450
#extension GL_GOOGLE_include_directive : require
#include "world_color.glsl"
layout(set=0,binding=0,std430) readonly buffer Quad {uint words[];} quad;
layout(set=0,binding=2) uniform sampler2DArray exposure_image;
layout(location=0) invariant out vec4 scene_uv;
layout(location=1) invariant out vec4 screen_exposure;
layout(location=2) invariant out vec4 dither_seed;

uint Endian(uint x) {
    uint mode=parameters.rules.x;
    if(mode==1 || mode==2)x=((x>>8)&0x00ff00ffu)|((x&0x00ff00ffu)<<8);
    if(mode==2 || mode==3)x=(x>>16)|(x<<16);
    return x;
}
struct Corner {vec4 position;vec4 uv;vec4 screen;vec4 seed;};
Corner LoadCorner(uint index) {
    vec4 data=uintBitsToFloat(uvec4(Endian(quad.words[index*4]),Endian(quad.words[index*4+1]),
        Endian(quad.words[index*4+2]),Endian(quad.words[index*4+3])));
    Corner result;
    precise vec2 uv=parameters.view[10].xx-data.zw;
    result.uv=vec4(uv,uv*parameters.view[1].xx+parameters.view[1].yz);
    precise float adaptation=textureLod(exposure_image,vec3(parameters.view[8].yy+
        parameters.exposure_bias_scale.xy,0),0).x*parameters.exposure_bias_scale.z;
    // Piecewise adaptation curve. The inputs describe three adjacent ranges.
    float low,high,first,last;
    if(adaptation<parameters.view[2].y) {
        low=parameters.view[2].x;high=parameters.view[2].y;
        first=parameters.view[3].x;last=parameters.view[3].y;
    }else if(adaptation>=parameters.view[2].z) {
        low=parameters.view[2].z;high=parameters.view[2].w;
        first=parameters.view[3].z;last=parameters.view[3].w;
    }else {
        low=parameters.view[2].y;high=parameters.view[2].z;
        first=parameters.view[3].y;last=parameters.view[3].z;
    }
    precise float amount=clamp((adaptation-low)/(high-low),0,1);
    precise float bias=parameters.view[5].y*parameters.view[5].x;
    precise float numerator=(bias+parameters.view[7].x)*parameters.view[5].x+parameters.view[7].z;
    precise float denominator=(bias+parameters.view[5].z)*parameters.view[5].x+parameters.view[7].y;
    precise float white=numerator/denominator-parameters.view[7].w;
    precise float exposure=(last-first)*amount+(first+parameters.view[6].w);
    result.screen=vec4(uv*parameters.view[1].xx+parameters.view[8].xx+parameters.view[1].yz,white,exposure);
    precise vec2 radial=parameters.view[8].zz-data.wz;
    precise float radius=radial.x*radial.x+radial.y*radial.y;
    precise float seed=radius+parameters.view[4].y;
    result.seed=seed*parameters.view[8].wwww+parameters.view[9];
    result.position=vec4(data.xy*parameters.view[0].xx,1,1);
    result.position.xyz=result.position.xyz*parameters.ndc_scale.xyz+parameters.ndc_offset.xyz;
    return result;
}
void main() {
    uint index=uint(gl_VertexIndex);
    Corner result;
    if(index<3)result=LoadCorner(index);
    else {
        Corner a=LoadCorner(0),b=LoadCorner(1),c=LoadCorner(2);
        // Preserve the rectangle's two triangles, including interpolation of
        // large dither seeds. Re-evaluating a fourth corner changes the noise.
        result.position=b.position+(c.position-a.position);
        result.uv=b.uv+(c.uv-a.uv);
        result.screen=b.screen+(c.screen-a.screen);
        precise vec4 delta=c.seed-a.seed;
        precise vec4 fourth_seed=b.seed+delta;
        result.seed=fourth_seed;
    }
    gl_Position=result.position;
    scene_uv=result.uv;screen_exposure=result.screen;dither_seed=result.seed;
}
