// UI-dev sandbox background: a clean vertical gradient with a soft vignette, so the ui-dev scene has
// a neutral flat-design backdrop (no geometry) to author UI against. Fullscreen triangle; outputs
// linear HDR (the composite tonemaps + re-encodes to sRGB). Depth off. Brief 05, milestone 0.

struct Push
{
    float2 screen_size;
    float  time;
    float  _pad;
};
[[vk::push_constant]]
Push pc;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;  // 0..1 across the screen (y down)
};

[shader("vertex")]
VSOut vertex_main(uint vertex_id : SV_VertexID)
{
    VSOut o;
    float2 ndc_uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    o.position = float4(ndc_uv * 2.0 - 1.0, 1.0, 1.0);
    o.uv = ndc_uv;  // (0,0) top-left .. (2,2), clipped to (0,0)..(1,1)
    return o;
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    // Flat-design gradient (authored directly in linear HDR): deep indigo top -> warmer bottom.
    float3 top = float3(0.055, 0.06, 0.10);
    float3 bot = float3(0.10, 0.09, 0.13);
    float3 col = lerp(top, bot, saturate(input.uv.y));

    // Soft radial vignette centred on screen.
    float2 p = input.uv - 0.5;
    float v = 1.0 - dot(p, p) * 0.9;
    col *= saturate(v);

    return float4(col, 1.0);
}
