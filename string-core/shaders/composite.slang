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
    uint lens_count;
    // THREE SEPARATE SCALARS, not a uint3. A uint3 has 16-byte alignment under std430, which would
    // push `lenses` to offset 48 here while the C++ struct puts it at 32 — the shader then reads
    // zeros, every lens rect has size 0, and the containment test rejects every pixel. Scalars pack
    // tightly at 4-byte alignment, so this matches the C++ layout exactly.
    uint _pad0;
    uint _pad1;
    uint _pad2;
    // Brief 14 M5, packed 16B per lens: .x = x | y<<16, .y = w | h<<16,
    // .z = source_slot | bypass<<16, .w = asuint(magnification).
    uint4 lenses[4];
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

// Brief 14 M5 — the lens.
//
// Returns true when `px` falls inside lens `i`, and fills the substituted source UV. The sampling is
//
//     src_uv = centre_uv + (px - centre) / (screen * magnification)
//
// which at magnification 1 reduces to identity — exact correspondence is STRUCTURAL, not tuned.
// CORRESPONDENCE MUST BE EXACT AT 1x, and algebra is not enough to get it. Writing the sampling as
//     centre/screen + (px - centre) / (screen * mag)
// reduces to px/screen at mag == 1 ALGEBRAICALLY, but not in floating point: the two divisions and
// the add do not round to the same bits, so every lens pixel samples a fraction of a texel off and a
// 1x lens visibly re-filters what it covers. This was caught by asserting the 1x interior is
// byte-identical to a no-lens capture — a test that "the pixels changed" would have passed.
//
// So the identity is STRUCTURAL: at 1x the caller keeps the interpolated uv untouched, and the
// remap runs only when actually magnifying.
bool lens_uv(uint i, float2 px, float2 screen, float2 base_uv, out float2 src_uv, out uint slot,
             out bool bypass)
{
    uint4 L = pc.lenses[i];
    float2 pos = float2(L.x & 0xFFFF, L.x >> 16);
    float2 size = float2(L.y & 0xFFFF, L.y >> 16);
    slot = L.z & 0xFFFF;
    bypass = (L.z >> 16) != 0;
    float mag = max(asfloat(L.w), 0.0001);

    src_uv = base_uv;
    if (size.x <= 0 || size.y <= 0) return false;
    if (any(px < pos) || any(px >= pos + size)) return false;

    // A 1x lens on the SCENE with no tonemap bypass substitutes nothing — it shows exactly what is
    // already there. Reporting "no match" makes it a true no-op: the caller never reassigns uv, so
    // those pixels take the byte-identical path they would without any lens.
    //
    // Merely returning the untouched uv here was NOT enough. Assigning it still put the sample
    // inside divergent control flow, and the result came out 1/255 different on ~10%% of the
    // interior. Caught by asserting a 1x lens is byte-identical to no lens, with the profiler HUD as
    // a control to prove the residual was the lens and not generic debug-UI noise.
    if (mag == 1.0 && slot == 0 && !bypass) return false;
    if (mag == 1.0) return true;

    float2 centre = pos + size * 0.5;
    src_uv = centre / screen + (px - centre) / (screen * mag);
    return true;
}

[shader("fragment")]
float4 fragment_main(VSOut input) : SV_Target
{
    float2 screen;
    textures[pc.source_slot].GetDimensions(screen.x, screen.y);

    float2 uv = input.uv;
    bool bypass_tonemap = false;
    uint src_slot = pc.source_slot;

    // LAST match wins, so a newer lens draws over an older one — painter order, matching how the
    // panels they are dragged with already behave.
    for (uint i = 0; i < pc.lens_count; ++i)
    {
        float2 luv; uint lslot; bool lbypass;
        if (!lens_uv(i, input.position.xy, screen, input.uv, luv, lslot, lbypass)) continue;
        uv = luv;
        bypass_tonemap = lbypass;
        src_slot = lslot == 0 ? pc.source_slot : lslot;

        // MAGNIFIED SAMPLING MUST SNAP TO TEXEL CENTRES. A linear sampler at 4x smears exactly the
        // texel-level detail being inspected — AA artifacts and fireflies ARE that detail. This
        // makes the existing linear sampler behave as nearest; no second sampler binding needed.
        if (asfloat(pc.lenses[i].w) > 1.0)
        {
            float2 tex;
            textures[src_slot].GetDimensions(tex.x, tex.y);
            uv = (floor(uv * tex) + 0.5) / tex;
        }
    }

    float3 hdr = textures[src_slot].Sample(uv).rgb * pc.exposure;
    // Bypass = SKIP THE TONEMAP CURVE, not skip exposure. A debug target holding raw data (normals,
    // roughness) must not be pushed through the output transform, which would turn it into a
    // plausible-looking WRONG image — hard to spot precisely because the result still looks like an
    // image.
    //
    // But dropping exposure too was wrong: the scene target is scene-referred kilo-units, so the raw
    // values are enormous and saturate() clamped the sky to flat white. Exposure is what brings a
    // scene-referred buffer into the 0..1 display range; the TONEMAP is the part that reshapes it.
    // Keeping exposure gives a linear, un-tonemapped view — highlights still clip (that is what
    // un-tonemapped MEANS), but everything below clipping is readable instead of uniformly white.
    if (bypass_tonemap)
        return float4(saturate(hdr), 1.0);
    float3 display = sample_lut(hdr);
    // Output dither: +-0.5 encoded LSB breaks the 8-bit quantization contours that band smooth
    // gradients (the procedural sky was the visible case). Sub-LSB amplitude — invisible as
    // noise, only redistributes the rounding.
    float n = ign(input.position.xy) - 0.5;
    display = srgb_decode(saturate(srgb_encode(display) + n / 255.0));
    return float4(display, 1.0);
}
