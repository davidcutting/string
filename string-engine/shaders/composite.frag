#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Samples the offscreen HDR target out of the bindless table and writes it to the
// swapchain. Set 0 is the global DescriptorTable; binding 1 is its combined-image-sampler
// array. The source slot arrives via push constant.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(push_constant) uniform Push
{
    uint source_slot;
} pc;

void main()
{
    out_color = texture(textures[nonuniformEXT(pc.source_slot)], uv);
}
