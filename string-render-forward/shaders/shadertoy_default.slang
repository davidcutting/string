// Brief 18 M4 — the shader a Shadertoy scene runs when the descriptor names no other file, and the
// template to copy when writing one.
//
// THE CONTRACT, which is the whole API:
//   - `Push pc` below, exactly as declared. It is the only binding; no descriptors, no set layout.
//   - a `vertex_main` that emits the fullscreen triangle (copy it verbatim — the pass issues
//     draw(3,1,0,0) and nothing else).
//   - a `fragment_main` returning the final colour.
//
// COLOUR SPACE — read this before porting a Shadertoy. Post (exposure/bloom/GTAO) is bypassed, but
// the composite pass still tonemaps (ACES) and encodes sRGB, so what you return here is
// SCENE-REFERRED LINEAR, the same as ui_background.slang. A Shadertoy shader's display-referred 0..1
// will therefore look washed out if pasted in unchanged. Full passthrough needs composite bypassed
// too, which is a renderer-side switch that does not exist yet (brief 18 M4, remaining work).
//
// Editing this file, or whichever file the scene names, hot-reloads it. A compile error leaves the
// last working version on screen and shows the diagnostic — you cannot break the session with a typo.

struct Push
{
    float2 resolution;   // iResolution, pixels
    float  time;         // iTime, seconds since the scene loaded
    float  time_delta;   // iTimeDelta, seconds
    float4 mouse;        // iMouse: xy = position, zw = click (not yet wired — always 0)
    int    frame;        // iFrame
    float3 _pad;
};
[[vk::push_constant]]
Push pc;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;   // 0..1 across the screen, y down
};

[shader("vertex")]
VSOut vertex_main(uint vertex_id : SV_VertexID)
{
    VSOut o;
    float2 ndc_uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    o.position = float4(ndc_uv * 2.0 - 1.0, 1.0, 1.0);
    o.uv = ndc_uv;
    return o;
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    // Aspect-corrected coordinates centred on the screen, the usual Shadertoy starting point.
    float2 p = (input.uv * 2.0 - 1.0);
    p.x *= pc.resolution.x / max(pc.resolution.y, 1.0);

    // Something that is obviously alive, so "is it running?" is answerable at a glance.
    float r = length(p);
    float a = atan2(p.y, p.x);
    float rings = 0.5 + 0.5 * sin(r * 12.0 - pc.time * 2.0 + sin(a * 3.0));
    float glow = smoothstep(1.2, 0.0, r);

    float3 col = lerp(float3(0.06, 0.07, 0.12), float3(0.25, 0.65, 0.95), rings * glow);
    return float4(col, 1.0);
}
