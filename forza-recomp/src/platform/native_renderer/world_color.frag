#version 450
#extension GL_GOOGLE_include_directive : require
#include "world_color.glsl"
layout(set=0,binding=3) uniform sampler2DArray scene_linear;
layout(set=0,binding=4) uniform sampler2DArray scene_point;
layout(set=0,binding=5) uniform sampler2DArray noise_image;
layout(set=0,binding=6) uniform sampler2DArray bloom_image;
layout(set=0,binding=7) uniform sampler3D grading_image;
layout(location=0) in vec4 scene_uv;
layout(location=1) in vec4 screen_exposure;
layout(location=2) in vec4 dither_seed;
layout(location=0) out vec4 output_color;
vec4 Scene(vec2 uv) {
    return textureLod(scene_linear,vec3(uv+parameters.scene_noise_bias.xy,0),0)*parameters.input_scale.x;
}
vec4 SelectSample(vec4 candidate,vec4 center,vec4 fallback) {
    return candidate.a==center.a?candidate:fallback;
}
vec3 PwlGamma(vec3 value) {
    value=clamp(value,0,1);
    bvec3 a=greaterThanEqual(value,vec3(64.0/1023));
    bvec3 b=greaterThanEqual(value,vec3(128.0/1023));
    bvec3 c=greaterThanEqual(value,vec3(512.0/1023));
    vec3 slope=mix(mix(vec3(1023),vec3(511.5),a),mix(vec3(255.75),vec3(127.875),c),b);
    vec3 offset=mix(mix(vec3(0),vec3(32.0/255),a),mix(vec3(64.0/255),vec3(128.0/255),c),b);
    return trunc(value*slope)/255+offset;
}
void main() {
    vec2 uv=scene_uv.xy;
    precise vec2 noise=textureLod(noise_image,vec3(uv+parameters.scene_noise_bias.zw,0),0).xy*
        parameters.input_scale.y*parameters.color[15].w;
    precise vec3 seed=fract((dither_seed.xyz*dither_seed.xyz).xzy);
    seed=seed*parameters.color[15].www+parameters.color[13].xxx;
    precise vec3 jitter=seed*parameters.color[10].zzz;
    precise vec4 ring=vec4(noise.yx,jitter.zz)+parameters.color[15].xxyz;
    precise vec2 direction=ring.yx*parameters.color[10].xx;
    precise vec4 outer=direction.yxxy*(parameters.color[14].xxyy+jitter.yyyy)+uv.yxxy;
    precise vec4 inner=direction.yxxy*ring.zzww+uv.yxxy;
    vec4 center=textureLod(scene_point,vec3(uv+parameters.scene_noise_bias.xy,0),0)*parameters.input_scale.x;
    vec4 mid=SelectSample(Scene(direction*jitter.xx+uv),center,center);
    vec4 inner_a=SelectSample(Scene(inner.yx),center,mid);
    vec4 inner_b=SelectSample(Scene(inner.zw),center,mid);
    vec4 outer_a=SelectSample(Scene(outer.zw),center,inner_b);
    vec4 outer_b=SelectSample(Scene(outer.yx),center,inner_a);
    precise vec3 filtered=outer_b.rgb*outer_b.rgb+outer_a.rgb*outer_a.rgb;
    filtered+=mid.rgb*mid.rgb;
    filtered+=inner_b.rgb*inner_b.rgb;
    filtered+=inner_a.rgb*inner_a.rgb;
    precise vec3 bloom=textureLod(bloom_image,vec3(scene_uv.zw+parameters.bloom_lut_bias.xy,0),0).rgb*
        parameters.input_scale.z;
    precise vec3 radiance=filtered*parameters.color[15].www+bloom*bloom;
    precise float exposure=exp2(screen_exposure.w);
    precise vec3 exposed=radiance*exposure;
    precise vec3 numerator=exposed*(parameters.color[11].yyy*exposure*radiance+parameters.color[12].xxx)+parameters.color[12].zzz;
    precise vec3 denominator=exposed*(parameters.color[11].yyy*exposure*radiance+parameters.color[11].zzz)+parameters.color[12].yyy;
    precise vec3 mapped=numerator/denominator-parameters.color[12].www;
    precise vec3 grading_uv=mapped*(parameters.color[4].xxx/screen_exposure.zzz)+parameters.color[5].xxx;
    vec3 graded=textureLod(grading_image,grading_uv+parameters.bloom_lut_bias.zzz,0).rgb*parameters.input_scale.w;
    precise vec2 shifted=screen_exposure.xy+parameters.color[3].xy;
    precise vec4 transformed=shifted.xxyy*parameters.color[0].xzyw;
    precise vec2 radial=transformed.xy+transformed.zw;
    precise float squared=radial.x*radial.x+radial.y*radial.y+parameters.color[13].y;
    precise float vignette=clamp(exp2(log2(abs(squared))*parameters.color[1].w),0,1);
    vec3 source=parameters.color[6].x>uv.x?graded:mapped/screen_exposure.zzz;
    source=sqrt(abs(source));
    vec3 flash=clamp(source+parameters.color[14].www+parameters.color[2].xyz,0,1);
    precise vec3 tinted=source+(flash-source)*(vignette*parameters.color[2].w);
    precise vec3 linear=tinted*tinted;
    precise float red=(linear.b*parameters.color[7].b+linear.r*parameters.color[7].r)+linear.g*parameters.color[7].g;
    precise float green=(linear.b*parameters.color[8].b+linear.r*parameters.color[8].r)+linear.g*parameters.color[8].g;
    precise float blue=(linear.b*parameters.color[9].b+linear.r*parameters.color[9].r)+linear.g*parameters.color[9].g;
    precise vec3 rgb=sqrt(clamp(vec3(red+parameters.color[7].w,
        green+parameters.color[8].w*parameters.color[14].z,blue+parameters.color[9].w*parameters.color[14].z),0,1));
    if(parameters.rules.y!=0)rgb=PwlGamma(rgb);
    output_color=vec4(rgb,1);
}
