// Instanced-quad texture blit for the UI's image widget (brief 14 M4).
//
// One instanced quad per image element. The instance record carries the destination rect, the
// bindless texture slot, and the INSPECTION controls — channel mask, explicit mip, range remap and
// false colour. Those exist because a raw blit is useless for debugging: a depth target reads as
// flat white, an SDF atlas as flat grey, and a packed target hides three of its four channels.
//
// Sampling is the same path the text shader already uses (bindless `Sampler2D textures[]` indexed by
// a slot the renderer resolves), which is why this could be built and verified against the glyph
// atlas with no render-target barrier risk at all.

struct Image
{
    float4 rect;     // xy = pos (px, top-left), zw = size (px)
    float4 range;    // x = range_min, y = range_max, z = mip, w = false_colour
    uint4  params;   // x = texture slot, y = channel mask (1=R 2=G 4=B 8=A), zw = unused
    float4 tint;     // rgba multiplier, linear
};

[[vk::binding(0, 0)]]
StructuredBuffer<Image> image_buffers[];

[[vk::binding(1, 0)]]
Sampler2D textures[];

struct Push
{
    float2 screen_size;
    uint image_slot;
    uint first;   // base index of this sub-draw's range (the overlay layer draws a tail range)
    // 1/exposure_scale: cancels the composite's EV100 exposure so the inspected image keeps the
    // values it actually holds rather than being re-graded by scene brightness (see ui_shader).
    float inv_exposure;
};
[[vk::push_constant]]
Push pc;

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint instance : TEXCOORD1;
};

static const float2 corners[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(1, 0), float2(1, 1), float2(0, 1),
};

[shader("vertex")]
VSOut vertex_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    Image im = image_buffers[pc.image_slot][pc.first + instance_id];

    float2 corner = corners[vertex_id];
    float2 pixel = im.rect.xy + corner * im.rect.zw;
    float2 ndc = (pixel / pc.screen_size) * 2.0 - 1.0;

    VSOut o;
    o.position = float4(ndc, 0.0, 1.0);
    o.uv = corner;                      // the element's box maps to the whole texture
    o.instance = pc.first + instance_id;
    return o;
}

// Viridis-ish ramp: perceptually monotonic, so ORDER is readable rather than just difference. A
// rainbow ramp reverses lightness partway and invents edges that are not in the data.
float3 false_colour(float t)
{
    const float3 c0 = float3(0.267, 0.005, 0.329);
    const float3 c1 = float3(0.188, 0.408, 0.557);
    const float3 c2 = float3(0.208, 0.718, 0.472);
    const float3 c3 = float3(0.993, 0.906, 0.144);
    t = saturate(t);
    if (t < 0.333) return lerp(c0, c1, t / 0.333);
    if (t < 0.666) return lerp(c1, c2, (t - 0.333) / 0.333);
    return lerp(c2, c3, (t - 0.666) / 0.334);
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    Image im = image_buffers[pc.image_slot][input.instance];

    // Explicit mip: the whole point of a mip control is to see a LEVEL, so an implicit derivative
    // (which would pick its own) is wrong here.
    float4 texel = textures[im.params.x].SampleLevel(input.uv, im.range.z);

    // Range remap first, so the mask selects from already-normalised values. A degenerate range
    // (max <= min) would divide by zero; treat it as identity rather than producing NaN.
    float span = im.range.y - im.range.x;
    float4 v = span > 1e-6 ? (texel - im.range.x) / span : texel;

    uint mask = im.params.y;
    float3 rgb;
    float alpha = 1.0;
    if (im.range.w > 0.5)
    {
        // False colour reads ONE scalar. Use the first masked-in channel so the control composes
        // with the channel mask instead of fighting it.
        float scalar = (mask & 1) != 0 ? v.r : ((mask & 2) != 0 ? v.g : ((mask & 4) != 0 ? v.b : v.a));
        rgb = false_colour(scalar);
    }
    else
    {
        rgb = float3((mask & 1) != 0 ? v.r : 0.0,
                     (mask & 2) != 0 ? v.g : 0.0,
                     (mask & 4) != 0 ? v.b : 0.0);
        // A single masked-in colour channel is shown as GREY, not as a red/green/blue tint —
        // isolating a channel is asking "what is in it", and a colour cast makes values harder to
        // compare between channels.
        if (mask == 1) rgb = v.rrr;
        else if (mask == 2) rgb = v.ggg;
        else if (mask == 4) rgb = v.bbb;
        else if (mask == 8) rgb = v.aaa;
        else if ((mask & 8) != 0) alpha = saturate(v.a);
    }

    return float4(saturate(rgb) * im.tint.rgb * pc.inv_exposure, alpha * im.tint.a);
}
