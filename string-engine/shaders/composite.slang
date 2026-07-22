// Composite/resolve pass, ported from composite.{vert,frag}.
//
// Samples the offscreen HDR target out of the bindless table, applies exposure + an ACES filmic
// tonemap, and writes to the swapchain. The swapchain is an sRGB format, so the hardware does the
// linear->sRGB encode on present — we output linear (tonemapped) here, no manual encode.
//
// The bindless combined-image-sampler array lives at set 0, binding 1 (matching descriptor_table).

[[vk::binding(1, 0)]]
Sampler2D textures[];

struct Push
{
    uint source_slot;
    float exposure;
};
[[vk::push_constant]]
Push pc;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle generated from the vertex index — no vertex buffers.
[shader("vertex")]
VSOut vertex_main(uint vertex_id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// ACES filmic approximation (Narkowicz 2015): maps HDR radiance to a display-referred [0,1] curve.
float3 aces(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    float3 hdr = textures[pc.source_slot].Sample(input.uv).rgb * pc.exposure;
    return float4(aces(hdr), 1.0);
}
