#version 450
#extension GL_EXT_nonuniform_qualifier : require

// One instanced quad per glyph. Glyphs live in a bindless storage buffer (set 0, binding 0)
// selected by the push-constant slot; each instance emits a rect in pixel space (converted to
// Vulkan NDC, y-down so pixel (0,0) = top-left) and its atlas UV sub-rect. Mirrors ui_shader.vert.
struct Glyph {
    vec4 rect;   // xy = pos (px, top-left), zw = size (px)
    vec4 uv;     // u0, v0, u1, v1  (atlas UVs)
    vec4 color;  // rgba, linear
};

layout(set = 0, binding = 0) readonly buffer GlyphBuffer {
    Glyph glyphs[];
} glyph_buffers[];

layout(push_constant) uniform Push {
    vec2 screen_size;
    uint glyph_slot;
    uint atlas_slot;
} pc;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out vec4 frag_color;

void main() {
    const vec2 corners[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(1, 0), vec2(1, 1), vec2(0, 1)
    );

    Glyph g = glyph_buffers[nonuniformEXT(pc.glyph_slot)].glyphs[gl_InstanceIndex];

    vec2 corner = corners[gl_VertexIndex];
    vec2 pixel = g.rect.xy + corner * g.rect.zw;
    vec2 ndc = (pixel / pc.screen_size) * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_uv = mix(g.uv.xy, g.uv.zw, corner);  // interpolate u0v0..u1v1 across the quad
    frag_color = g.color;
}
