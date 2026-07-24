// Composite/resolve pass.
//
// Samples the offscreen HDR target out of the bindless table, applies exposure, then the brief-09
// baked output-transform LUT (HDR grading + the ACES-2.0-style CAM DRT, or the ACES-v1 fitted
// curve behind r.tonemap=aces1 — both baked CPU-side into the same LUT), and writes to the
// swapchain. The swapchain is an sRGB format, so the hardware does the linear->sRGB encode.
//
// LUT: a 3D table stored as a 2D strip (lut_size slices of lut_size x lut_size, side by side).
// Input domain is the log2 shaper from tonemap.hpp — keep kLog2Min/Max in lockstep with it.
// Runtime cost: two bilinear fetches + one lerp (manual trilinear).
//
// The bindless combined-image-sampler array lives at set 0, binding 1 (matching descriptor_table).

[[vk::binding(1, 0)]]
Sampler2D textures[];

struct Push
{
    uint source_slot;
    float exposure;
    uint lut_slot;
    uint lut_size;
};
[[vk::push_constant]]
Push pc;

static const float kLog2Min = -12.0;
static const float kLog2Max = 7.0;

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

float3 shaper_encode(float3 c)
{
    float3 l = log2(max(c, exp2(kLog2Min)));
    return saturate((l - kLog2Min) / (kLog2Max - kLog2Min));
}

// Manual trilinear on the 2D strip: bilinear within the two neighbouring blue slices, lerped.
float3 sample_lut(float3 exposed)
{
    float3 enc = shaper_encode(exposed);
    float n = float(pc.lut_size);
    float slice = enc.z * (n - 1.0);
    float s0 = floor(slice);
    float sf = slice - s0;
    float s1 = min(s0 + 1.0, n - 1.0);
    // Within-slice texel-center coordinates (red across a slice, green down); each slice is a
    // 1/n-wide band of the strip.
    float u = (enc.x * (n - 1.0) + 0.5) / n;   // fraction of one slice's width
    float v = (enc.y * (n - 1.0) + 0.5) / n;
    float3 a = textures[pc.lut_slot].SampleLevel(float2((s0 + u) / n, v), 0).rgb;
    float3 b = textures[pc.lut_slot].SampleLevel(float2((s1 + u) / n, v), 0).rgb;
    return lerp(a, b, sf);
}

// sRGB encode/decode pair for the output dither: the swapchain stores 8-bit sRGB, so quantization
// happens in the ENCODED domain — the dither must be one ENCODED LSB, applied there and mapped
// back to the linear value the hardware will re-encode.
float3 srgb_encode(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}
float3 srgb_decode(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(max(c + 0.055, 0.0) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

// Interleaved gradient noise: deterministic per-pixel (SPATIAL only — no frame term, so the
// no-TAA rule and the run-twice determinism gate hold).
float ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    float3 hdr = textures[pc.source_slot].Sample(input.uv).rgb * pc.exposure;
    float3 display = sample_lut(hdr);
    // Output dither: +-0.5 encoded LSB breaks the 8-bit quantization contours that band smooth
    // gradients (the procedural sky was the visible case). Sub-LSB amplitude — invisible as
    // noise, only redistributes the rounding.
    float n = ign(input.position.xy) - 0.5;
    display = srgb_decode(saturate(srgb_encode(display) + n / 255.0));
    return float4(display, 1.0);
}
