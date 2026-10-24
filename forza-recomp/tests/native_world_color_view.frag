#version 450
layout(location=1) in vec4 screen_exposure;
layout(location=0) out vec4 output_color;
void main(){output_color=screen_exposure;}
