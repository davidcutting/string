// Instanced-quad SDF text, ported from text_shader.{vert,frag}.
//
// One instanced quad per glyph. Glyphs live in a bindless storage buffer (set 0, binding 0)
// selected by the push-constant slot; each instance emits a pixel-space rect (converted to Vulkan
// NDC, y-down) and its atlas UV sub-rect. The atlas (a bindless R8 image at set 0, binding 1,
// selected by atlas_slot) stores a signed distance field: the glyph edge sits at ~0.5, thresholded
// with a screen-space-derivative-wide smoothstep so text stays crisp at any scale.

struct Glyph
{
    float4 rect;   // xy = pos (px, top-left), zw = size (px)
    float4 uv;     // u0, v0, u1, v1  (atlas UVs)
    float4 color;  // rgba, linear
};

[[vk::binding(0, 0)]]
StructuredBuffer<Glyph> glyph_buffers[];

[[vk::binding(1, 0)]]
Sampler2D textures[];

struct Push
{
    float2 screen_size;
    uint glyph_slot;
    uint atlas_slot;
    uint first;   // base index of this sub-draw's range (the overlay layer draws a tail range)
    // 1/exposure_scale: cancels the composite's EV100 exposure so display-referred text keeps its
    // authored brightness regardless of scene EV (see ui_shader.slang).
    float inv_exposure;
};
[[vk::push_constant]]
Push pc;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

static const float2 corners[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(1, 0), float2(1, 1), float2(0, 1),
};

[shader("vertex")]
VSOut vertex_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    Glyph g = glyph_buffers[pc.glyph_slot][pc.first + instance_id];

    float2 corner = corners[vertex_id];
    float2 pixel = g.rect.xy + corner * g.rect.zw;
    float2 ndc = (pixel / pc.screen_size) * 2.0 - 1.0;

    VSOut o;
    o.position = float4(ndc, 0.0, 1.0);
    o.uv = lerp(g.uv.xy, g.uv.zw, corner);  // interpolate u0v0..u1v1 across the quad
    o.color = float4(g.color.rgb * pc.inv_exposure, g.color.a);
    return o;
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    float d = textures[pc.atlas_slot].Sample(input.uv).r;  // SDF, edge at ~0.5
    float aa = fwidth(d);                                  // ~half a texel of distance
    float coverage = smoothstep(0.5 - aa, 0.5 + aa, d);
    float alpha = input.color.a * coverage;
    if (alpha <= 0.0)
    {
        discard;
    }
    return float4(input.color.rgb, alpha);
}
