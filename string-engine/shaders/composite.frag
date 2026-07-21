#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Samples the offscreen HDR target out of the bindless table, applies exposure + an ACES filmic
// tonemap, and writes to the swapchain. The swapchain is an sRGB format, so the hardware does the
// linear->sRGB encode on present — we output linear (tonemapped) here, no manual encode.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(push_constant) uniform Push
{
    uint source_slot;
    float exposure;
} pc;

// ACES filmic approximation (Narkowicz 2015): maps HDR radiance to a display-referred [0,1] curve.
vec3 aces(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 hdr = texture(textures[nonuniformEXT(pc.source_slot)], uv).rgb * pc.exposure;
    out_color = vec4(aces(hdr), 1.0);
}
