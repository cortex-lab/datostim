#version 450

layout(std140, binding = 0) uniform Params { vec4 color; }
params;

layout(location = 0) out vec4 out_color;

void main() { out_color = params.color; }
