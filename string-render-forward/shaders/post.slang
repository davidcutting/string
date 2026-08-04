// Brief 09 post-processing compute chain: luminance-histogram auto-exposure metering + the
// tight/punchy Karis bloom (threshold-free downsample/upsample pyramid, artistic clamp knobs).
// All entry points share one Push; the pass dispatches them in sequence with local barriers
// (intra-pass transient chain — the bloom pyramid + histogram live entirely inside this pass).
//
// Runs on the RESOLVED single-sample HDR target (post-MSAA-resolve, pre-composite); the apply
// step writes bloom back INTO that target in place, so the composite and the capture writer both
// see the final HDR frame.

[[vk::binding(1, 0)]] Sampler2D src_textures[];          // sampled: scene color / bloom chain
[[vk::binding(2, 0)]] RWTexture2D<float4> dst_images[];  // storage: bloom mips / scene color

struct HistBuffer { uint bins[]; };                       // 256 bins
struct ExposureOut { float avg_log_lum; uint total; uint kept; float _pad; };

struct Push
{
    HistBuffer *hist;        // 0
    ExposureOut *readback;   // 8
    uint src_slot;           // 16
    uint dst_slot;           // 20
    uint2 src_size;          // 24
    uint2 dst_size;          // 32
    uint flags;              // 40  bit0: first downsample (Karis average + firefly clamp);
                             //     bits 8..15: source mip level
    float p0;                // 44  down: clamp_max | reduce: cut_low | up/apply: intensity
    float p1;                // 48  reduce: cut_high
    float log_min;           // 52  histogram log2-luminance domain
    float log_inv_range;     // 56  1 / (log_max - log_min)
    uint _pad;               // 60
};
[[vk::push_constant]] Push pc;

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// --- Auto-exposure: luminance histogram ----------------------------------------------------------

[shader("compute")]
[numthreads(256, 1, 1)]
void hist_clear_main(uint3 id : SV_DispatchThreadID)
{
    pc.hist.bins[id.x] = 0;
}

groupshared uint lh[256];

[shader("compute")]
[numthreads(16, 16, 1)]
void hist_main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    lh[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < pc.src_size.x && id.y < pc.src_size.y)
    {
        float2 uv = (float2(id.xy) + 0.5) / float2(pc.src_size);
        float3 c = src_textures[pc.src_slot].SampleLevel(uv, 0).rgb;
        float lum = dot(max(c, float3(0.0)), kLuma);
        uint bin = 0;
        if (lum > 1e-5)
            bin = clamp(1u + uint((log2(lum) - pc.log_min) * pc.log_inv_range * 254.0), 1u, 255u);
        InterlockedAdd(lh[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lh[gi] != 0) InterlockedAdd(pc.hist.bins[gi], lh[gi]);
}

// Percentile-trimmed mean of the log2 luminance: drop the darkest cut_low and brightest cut_high
// fractions (sky/sun and pitch shadow shouldn't drag the metering), average the rest. Single
// workgroup; thread 0 walks the 256 bins serially (trivial work).
[shader("compute")]
[numthreads(1, 1, 1)]
void hist_reduce_main()
{
    uint total = 0;
    for (uint i = 0; i < 256; ++i) total += pc.hist.bins[i];
    const float cut_low = pc.p0, cut_high = pc.p1;
    float lo = float(total) * cut_low;
    float hi = float(total) * (1.0 - cut_high);
    float sum = 0.0, kept = 0.0, seen = 0.0;
    for (uint i = 0; i < 256; ++i)
    {
        float n = float(pc.hist.bins[i]);
        float keep = clamp(min(seen + n, hi) - max(seen, lo), 0.0, n);
        // bin 0 = "below range": count it at the domain floor so black frames still meter.
        float bin_log = i == 0 ? pc.log_min
                               : pc.log_min + (float(i) - 0.5) / (254.0 * pc.log_inv_range);
        sum += keep * bin_log;
        kept += keep;
        seen += n;
    }
    pc.readback.avg_log_lum = kept > 0.0 ? sum / kept : pc.log_min;
    pc.readback.total = total;
    pc.readback.kept = uint(kept);
}

// --- Bloom: 13-tap downsample (Karis average on the first mip) + 3x3 tent upsample ---------------

float3 fetch(float2 uv, float lod)
{
    return src_textures[pc.src_slot].SampleLevel(uv, lod).rgb;
}

float3 karis(float3 c0, float3 c1, float3 c2, float3 c3)
{
    // Luma-weighted group average (partial Karis): suppresses single-sample fireflies without a
    // threshold — the physically-based chain the brief locked, kept tight by the clamp knob.
    float w0 = 1.0 / (1.0 + dot(c0, kLuma));
    float w1 = 1.0 / (1.0 + dot(c1, kLuma));
    float w2 = 1.0 / (1.0 + dot(c2, kLuma));
    float w3 = 1.0 / (1.0 + dot(c3, kLuma));
    return (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / (w0 + w1 + w2 + w3);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_down_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size.x || id.y >= pc.dst_size.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.dst_size);
    float2 ts = 1.0 / float2(pc.src_size);
    float lod = float((pc.flags >> 8) & 0xFF);
    bool first = (pc.flags & 1u) != 0u;

    float3 t[13];
    const float2 offs[13] = {
        float2(-2,-2), float2(0,-2), float2(2,-2),
        float2(-1,-1), float2(1,-1),
        float2(-2, 0), float2(0, 0), float2(2, 0),
        float2(-1, 1), float2(1, 1),
        float2(-2, 2), float2(0, 2), float2(2, 2),
    };
    [unroll] for (int i = 0; i < 13; ++i)
    {
        float3 c = max(fetch(uv + offs[i] * ts, lod), float3(0.0));
        if (first) c = min(c, float3(pc.p0));   // firefly/haze clamp (r.bloom.clamp, scene units)
        t[i] = c;
    }
    // Five overlapping 2x2 groups (weights 0.5 center + 4 x 0.125), Karis-averaged on the first
    // downsample, plain averages after (fireflies are gone once averaged at half res).
    float3 g0, g1, g2, g3, g4;
    if (first)
    {
        g0 = karis(t[3], t[4], t[8], t[9]);
        g1 = karis(t[0], t[1], t[5], t[6]);
        g2 = karis(t[1], t[2], t[6], t[7]);
        g3 = karis(t[5], t[6], t[10], t[11]);
        g4 = karis(t[6], t[7], t[11], t[12]);
    }
    else
    {
        g0 = (t[3] + t[4] + t[8] + t[9]) * 0.25;
        g1 = (t[0] + t[1] + t[5] + t[6]) * 0.25;
        g2 = (t[1] + t[2] + t[6] + t[7]) * 0.25;
        g3 = (t[5] + t[6] + t[10] + t[11]) * 0.25;
        g4 = (t[6] + t[7] + t[11] + t[12]) * 0.25;
    }
    float3 c = g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    dst_images[pc.dst_slot][int2(id.xy)] = float4(c, 1.0);
}

float3 tent9(float2 uv, float2 ts, float lod)
{
    float3 c = fetch(uv, lod) * 4.0;
    c += (fetch(uv + float2(-ts.x, 0), lod) + fetch(uv + float2(ts.x, 0), lod)
        + fetch(uv + float2(0, -ts.y), lod) + fetch(uv + float2(0, ts.y), lod)) * 2.0;
    c += fetch(uv + float2(-ts.x, -ts.y), lod) + fetch(uv + float2(ts.x, -ts.y), lod)
       + fetch(uv + float2(-ts.x, ts.y), lod) + fetch(uv + float2(ts.x, ts.y), lod);
    return c / 16.0;
}

// Upsample-accumulate: dst(mip m) += tent(mip m+1). p0 = per-step scale (radius/energy knob).
[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_up_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size.x || id.y >= pc.dst_size.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.dst_size);
    float2 ts = 1.0 / float2(pc.src_size);
    float lod = float((pc.flags >> 8) & 0xFF);
    float3 up = tent9(uv, ts, lod) * pc.p0;
    float4 cur = dst_images[pc.dst_slot][int2(id.xy)];
    dst_images[pc.dst_slot][int2(id.xy)] = float4(cur.rgb + up, 1.0);
}

// Final apply: scene += bloom * intensity (tent-filtered bilinear up from the half-res mip 0).
// The scene target is BOTH sampled state (histogram/downsample read it) and storage here; the
// pass's local barrier ordered those reads before this write.
[shader("compute")]
[numthreads(8, 8, 1)]
void bloom_apply_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size.x || id.y >= pc.dst_size.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.dst_size);
    float2 ts = 1.0 / float2(pc.src_size);
    float3 bloom = tent9(uv, ts, 0.0);
    float4 cur = dst_images[pc.dst_slot][int2(id.xy)];
    dst_images[pc.dst_slot][int2(id.xy)] = float4(cur.rgb + bloom * pc.p0, cur.a);
}
