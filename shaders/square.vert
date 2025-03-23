#version 450

layout(location = 0) in vec3 pos;

void main()
{
    gl_Position = vec4(pos, 1);
    gl_Position.y = -gl_Position.y; // HACK: Vulkan has y downwards
}
