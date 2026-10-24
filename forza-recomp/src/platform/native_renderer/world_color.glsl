layout(set=0,binding=1,std140) uniform Parameters {
    vec4 color[16];
    vec4 view[11];
    vec4 ndc_scale;
    vec4 ndc_offset;
    vec4 scene_noise_bias;
    vec4 bloom_lut_bias;
    vec4 input_scale;
    vec4 exposure_bias_scale;
    uvec4 rules;
} parameters;
