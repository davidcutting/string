// Brief 09b M2 — probe relight + convolve (RUNTIME compute, the dynamic part). One workgroup per
// probe, two phases:
//
//   PHASE 1 (cooperative): compute the outgoing radiance toward the probe for each of the 256
//   (16x16) octahedral CAPTURE directions from the static capture G-buffer:
//       miss  -> sky_env(dir)                               (the live sky, same units as the IBL)
//       hit   -> albedo * (sun_illum * NdotL * (1-shadow) / pi  +  bounce)
//   where shadow is a 1-tap CSM lookup at the hit point (probe resolution needs no PCF; hits
//   outside cascade range are treated as lit — the brief's documented v1 far-field limitation)
//   and bounce = probe_irradiance() of the PREVIOUS irradiance atlas at the hit point with the hit
//   normal — the standard DDGI trick: each relight pass propagates light one bounce further, so
//   repeated passes converge to infinite-bounce GI for free.
//
//   PHASE 2: each thread cosine-convolves the 256 samples into its 8x8(+gutter) irradiance texel:
//       E(N)/pi = sum_i L_i * max(N.dir_i, 0) * dOmega / pi,   dOmega = 4pi/256  =>  factor 4/256.
//   (Uniform L reconstructs exactly L — dimensionally identical to the sky-SH term it replaces.)
//   Blended with hysteresis for temporal stability; the DDGI gutter border rule fills the 1-texel
//   seam so hardware bilinear wraps the octahedron.
//
// The bounce SAMPLES the same atlas this dispatch WRITES (other probes' texels). Those reads are
// unordered against the writes within the dispatch and may see old or new values — both are valid
// iterates of the same fixed-point solve the hysteresis is already smoothing; DDGI implementations
// commonly accept this in place of ping-pong buffering.
import probe_common;
import lighting;   // SceneData (CSM cascades, view, sh), lit_textures bindless, sh_irradiance
import sky;        // sky_env (the miss term — the live sky, no sun disc)

static const float RL_PI = 3.14159265359;

// (ProbeActiveBuffer / ProbeOffsetBuffer come from probe_common — shared with sampling.)

[[vk::binding(2, 0)]] RWTexture2D<float4> images_2d[];   // irradiance atlas storage

struct Push
{
    float4 origin_spacing;   // xyz origin, w unused
    float4 spacing;          // xyz per-axis spacing
    uint4 counts;            // xyz counts, w total
    float4 sun_dir;          // xyz dir TO sun, w intensity (klx)
    float4 sun_color;        // xyz, w = hysteresis (fraction of previous atlas kept)
    float4 sky_zenith;       // xyz
    float4 sky_ground;       // xyz ground albedo
    uint cap_gbuf_slot;
    uint cap_albedo_slot;
    uint irrad_prev_slot;    // sampled slot of the irradiance atlas (bounce + hysteresis src)
    uint irrad_dst_slot;     // storage slot of the irradiance atlas (write)
    uint vis_slot;
    uint first_frame;        // 1 -> no hysteresis (initialize)
    uint probe_base;
    uint probe_count;
    ShBuffer *sh;            // sky SH (bounce fallback when the hit point has ~0 probe weight)
    ProbeActiveBuffer *active;    // per-probe activation (skip inactive probes -> leave atlas as-is)
    ProbeOffsetBuffer *offsets;   // per-probe relocation (hit points + bounce use relocated positions)
    SceneData *scene;        // this frame's CSM cascades + view (1-tap shadow at hit points)
};
[[vk::push_constant]] Push pc;

uint3 coord_of(uint linear)
{
    uint x = linear % pc.counts.x;
    uint y = (linear / pc.counts.x) % pc.counts.y;
    uint z = linear / (pc.counts.x * pc.counts.y);
    return uint3(x, y, z);
}

ProbeGrid make_grid()
{
    ProbeGrid g;
    g.origin = pc.origin_spacing.xyz;
    g.spacing = pc.spacing.xyz;
    g.counts = pc.counts.xyz;
    g.irrad_stride = 10;
    g.vis_stride = 18;
    g.offsets = pc.offsets;   // relocated positions everywhere
    g.active = pc.active;     // bounce sampling skips inactive probes too
    return g;
}

// 1-tap CSM sun shadow at an arbitrary world point (probe capture texels need no PCF). Outside every
// cascade -> lit (documented v1 far-field limitation). Mirrors lighting.slang's cascade selection.
float probe_sun_shadow(float3 world, float3 n, float ndl)
{
    SceneData *s = pc.scene;
    float view_z = mul(s.view, float4(world, 1.0)).z;
    float depth = -view_z;
    uint cascade = s.cascade_count - 1;
    for (uint i = 0; i < s.cascade_count; ++i)
        if (depth < s.cascade_split[i].x) { cascade = i; break; }
    float3 p = world + n * (s.cascade_texel[cascade].x * s.shadow_normal_offset_scale);
    float4 lp = mul(s.cascade_view_proj[cascade], float4(p, 1.0));
    float3 ndc = lp.xyz / max(lp.w, 1e-6);
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 0.0;   // outside cascade range -> lit
    float bias = s.shadow_bias * (1.0 + 2.0 * (1.0 - ndl));
    // NonUniformResourceIndex: `cascade` is chosen per hit point, so this bindless index diverges
    // across the relight compute wave -> UB without the qualifier (same class of bug as lighting.slang's
    // sample_cascade). Left unmarked, GI sun-shadow read the wrong cascade non-deterministically.
    uint slot = s.cascade_slot[cascade].x;
    float occluder = lit_textures[NonUniformResourceIndex(slot)].SampleLevel(uv, 0).r;
    return (ndc.z < occluder - bias) ? 1.0 : 0.0;
}

// Phase-1 products: per capture direction, its unit dir + incoming radiance at the probe.
static const uint kCapSamples = 16 * 16;
groupshared float3 gs_dir[kCapSamples];
groupshared float3 gs_rad[kCapSamples];

[shader("compute")]
[numthreads(10, 10, 1)]   // kProbeIrradStride^2 = one thread per irradiance tile texel (incl gutter)
void relight_main(uint3 id : SV_DispatchThreadID, uint3 lid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint stride = 10;      // kProbeIrradStride
    const uint inner = 8;        // kProbeIrradSize
    uint probe = pc.probe_base + gid.z;
    if (probe >= pc.counts.w) return;
    // Inactive probes (inside geometry) keep whatever the atlas holds — sampling down-weights them.
    // Workgroup-uniform, so returning before the barrier is safe.
    if (pc.active.state[probe] == 0u) return;

    ProbeGrid g = make_grid();
    uint3 c = coord_of(probe);
    float3 probe_pos = probe_world(g, c);   // relocated
    uint col = c.y * pc.counts.x + c.x;
    uint row = c.z;
    int2 irrad_origin = int2(col * stride, row * stride);
    int2 cap_inner_origin = int2(col * 18, row * 18) + int2(1, 1);   // vis/capture tile inner origin

    float2 cap_atlas = float2(probe_atlas_size(g, 18));
    float3 sun_L = normalize(pc.sun_dir.xyz);
    float3 sun_illum = pc.sun_color.xyz * pc.sun_dir.w;   // klx

    // --- PHASE 1: radiance per capture direction (100 threads cover 256 texels) -----------------
    uint tflat = lid.y * 10 + lid.x;
    for (uint i = tflat; i < kCapSamples; i += 100)
    {
        uint2 xy = uint2(i % 16, i / 16);
        float2 oct = (float2(xy) + 0.5) / 16.0 * 2.0 - 1.0;
        float3 dir = oct_decode(oct);

        float2 uv = (float2(cap_inner_origin + int2(xy)) + 0.5) / cap_atlas;
        float4 nd = lit_textures[pc.cap_gbuf_slot].SampleLevel(uv, 0);
        float3 L;
        if (nd.w < 0.0)
        {
            // Sky miss: the live sky's radiance (no sun disc — the analytic sun is the direct term).
            L = sky_env(dir, sun_L, pc.sky_zenith.xyz, pc.sky_ground.xyz, pc.sun_color.xyz,
                        pc.sun_dir.w);
        }
        else
        {
            float3 albedo = lit_textures[pc.cap_albedo_slot].SampleLevel(uv, 0).rgb;
            // The capture G-buffer is bilinearly filtered fp16, so a texel straddling a silhouette
            // can carry a cancelled/near-zero normal at a "hit". normalize() of that is non-finite;
            // fall back to facing the probe (-dir) so this direction still contributes sanely
            // instead of poisoning the whole tile.
            float nlen = length(nd.xyz);
            float3 hit_n = nlen > 1e-4 ? nd.xyz / nlen : -dir;
            float3 hit = probe_pos + dir * nd.w;
            float ndl = max(dot(hit_n, sun_L), 0.0);
            float3 direct = float3(0.0);
            if (ndl > 0.0 && pc.sun_dir.w > 0.0)
                direct = sun_illum * (ndl * (1.0 - probe_sun_shadow(hit, hit_n, ndl)) / RL_PI);
            // Bounce: previous irradiance at the hit point (E/pi — exactly the diffuse response of
            // a unit-albedo surface). Fallback to the sky SH when the probe weight there is ~0.
            float w = 0.0;
            float3 bounce = probe_irradiance(g, lit_textures[pc.irrad_prev_slot],
                                             lit_textures[pc.vis_slot], hit, hit_n, -dir, w);
            if (w <= 1e-5) bounce = sh_irradiance(pc.sh, hit_n);
            L = albedo * (direct + bounce);
        }
        // Firewall: phase 2 sums this over all 256 samples PER texel, so a single non-finite (or
        // negative) radiance would blacken every one of the probe's 64 irradiance texels — the
        // "black probe casting a dark shadow" artifact. Reject it here, at the one boundary where
        // radiance enters the atlas, so no upstream capture glitch can propagate.
        if (any(isnan(L)) || any(isinf(L))) L = float3(0.0);
        gs_dir[i] = dir;
        gs_rad[i] = max(L, float3(0.0));
    }
    GroupMemoryBarrierWithGroupSync();

    // --- PHASE 2: cosine-convolve into this thread's irradiance texel ---------------------------
    // Map this texel (including gutter) to an inner texel + octahedral direction. Gutter texels
    // (border) map to the mirrored inner edge (Cigolle/Majercik octa border rule).
    int2 local = int2(lid.xy);           // 0..9 within the tile
    int2 inner_xy = local - int2(1, 1);  // -1..8
    float2 oct;
    {
        bool2 lo = inner_xy < int2(0);
        bool2 hi = inner_xy >= int2(inner);
        int2 m = inner_xy;
        if (lo.x) { m.x = 0; m.y = inner - 1 - m.y; }
        if (hi.x) { m.x = inner - 1; m.y = inner - 1 - m.y; }
        if (lo.y) { m.y = 0; m.x = inner - 1 - m.x; }
        if (hi.y) { m.y = inner - 1; m.x = inner - 1 - m.x; }
        m = clamp(m, int2(0), int2(inner - 1));
        float2 f = (float2(m) + 0.5) / float(inner);
        oct = f * 2.0 - 1.0;
    }
    float3 N = oct_decode(oct);

    // E(N)/pi = sum L_i cos+ dOmega / pi; octa texels are ~equal solid angle => dOmega/pi = 4/256.
    float3 e = float3(0.0);
    for (uint i = 0; i < kCapSamples; ++i)
        e += gs_rad[i] * max(dot(N, gs_dir[i]), 0.0);
    e *= 4.0 / float(kCapSamples);

    if (any(isnan(e)) || any(isinf(e))) e = float3(0.0);
    e = max(e, float3(0.0));
    int2 texel = irrad_origin + local;
    float3 prev = images_2d[pc.irrad_dst_slot][texel].rgb;
    if (any(isnan(prev)) || any(isinf(prev))) prev = float3(0.0);   // don't let a stale poison persist
    float hyst = pc.first_frame != 0u ? 0.0 : pc.sun_color.w;
    images_2d[pc.irrad_dst_slot][texel] = float4(lerp(e, prev, hyst), 1.0);
}
