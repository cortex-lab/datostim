#version 450

// Varying.
layout(location = 0) in vec2 UV;

// Attachment output.
layout(location = 0) out vec4 color;


// Push constant.
layout(push_constant) uniform Push
{
    // WARNING: the variables should be sorted by decreasing size to avoid alignment issues.
    mat4 model;
    mat4 view;
    mat4 projection;

    vec4 min_color;
    vec4 max_color;
    vec2 tex_offset; /* offset the texture, degrees */
    vec2 tex_size;   /* size of the texture, degrees */
    float tex_angle; /* rotate the texture, degrees */

    // For fragment shader.
    /* float viewAngle;*/ /* rotation of view, degrees */
    /* vec2 pos;*/        /* position of layer [azimuth, altitude], degrees */
}
params;

// Descriptor slots.
layout(binding = 0) uniform sampler2D myTextureSampler;



void main()
{
    /*color = vec4(1.0f, 1.0f, 1.0f, 1.0f);*/
    /*color = fragmentColor;*/
    /*color = vec4(1.0f, 1.0f, 1.0f, 1.0f);*/
    /*vec2 scale;
    scale.x = 360/size.x;
    scale.y = 180/size.y;*/

    color = texture(myTextureSampler, UV).rgba;
    color = color * (params.max_color - params.min_color) + params.min_color;

    // DEBUG
    // vec4 max_color = vec4(1, 1, 0, 1);
    // vec4 min_color = vec4(1, 0, 1, 1);
    // color = color * (max_color - min_color) + min_color;

    // DEBUG
    // color = vec4(UV, 1, 1);
}
