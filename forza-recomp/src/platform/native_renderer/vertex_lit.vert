#version 450
layout(location=0) in vec4 position;
layout(location=1) in vec4 irradiance;
layout(location=0) invariant out vec4 vertex_irradiance;
void main() {gl_Position=position;vertex_irradiance=irradiance;}
