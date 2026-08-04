#version 450
#extension GL_EXT_nonuniform_qualifier : require

// SDF glyph rendering. The atlas (a bindless single-channel R8 image at set 0, binding 1, selected
// by the push-constant slot) stores a signed distance field: the glyph edge sits at ~0.5. We
// threshold with a screen-space-derivative-wide smoothstep, so text stays crisp at any scale (the
// point of SDF over a bitmap atlas). The pipeline does the alpha blend; color is already linear.
layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(push_constant) uniform Push {
    vec2 screen_size;
    uint glyph_slot;
    uint atlas_slot;
} pc;

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in vec4 frag_color;

layout(location = 0) out vec4 out_color;

void main() {
    float d = texture(textures[nonuniformEXT(pc.atlas_slot)], frag_uv).r;  // SDF, edge at ~0.5
    float aa = fwidth(d);                                                  // ~half a texel of distance
    float coverage = smoothstep(0.5 - aa, 0.5 + aa, d);
    float alpha = frag_color.a * coverage;
    if (alpha <= 0.0) {
        discard;
    }
    out_color = vec4(frag_color.rgb, alpha);
}
