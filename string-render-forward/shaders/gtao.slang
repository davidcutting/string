// Brief 09: half-res GTAO (Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate
// Indirect Occlusion") with BENT NORMAL output, + a depth-aware spatial denoise.
//
// Source depth is the PREVIOUS frame's min-resolved single-sample scene depth (the 04d hz.depth
// chain) with that frame's matrices; consumers reproject through SceneData.prev_view_proj. With a
// static scene this is exact under camera motion (world positions reproject into the exact depth
// buffer the AO was computed from); disocclusions fall back to AO = 1 for one frame (no temporal
// accumulation — nothing can ghost; the brief-locked no-TAA constraint).
//
// Normals are RECONSTRUCTED from depth (the brief's outline-slot preference — no G-buffer).
// Output (RGBA8): rgb = world-space bent normal * 0.5 + 0.5, a = cosine-weighted visibility.
// Multi-bounce is applied at CONSUMPTION (lighting.slang) where the real albedo is known.

[[vk::binding(1, 0)]] Sampler2D src_textures[];          // depth (nearest) / raw AO (linear)
[[vk::binding(2, 0)]] RWTexture2D<float4> dst_images[];  // half-res AO targets

struct Push
{
    float4x4 view;        // 0    prev frame world->view
    float4x4 inv_proj;    // 64   prev frame inverse projection (NDC -> view)
    uint depth_slot;      // 128  prev hz.depth sampled slot (full-res, single-sample, reverse-Z)
    uint dst_slot;        // 132
    uint2 dst_size;       // 136  half-res target size
    uint2 depth_size;     // 144
    float radius;         // 152  world-space sampling radius
    float proj00;         // 156  |proj[0][0]| / |proj[1][1]| — world->NDC scale at unit depth
    float proj11;         // 160
    uint src_slot;        // 164  denoise: raw AO input
    uint _pad0;           // 168
    uint _pad1;           // 172
};
[[vk::push_constant]] Push pc;

static const float PI = 3.14159265359;
static const uint kSlices = 3;
// 8 (raised from 5, 2026-07-24): with uniform spacing the largest horizon-sample gap is
// radius/kSteps; at 5 the residual discretization still banded the concave arcade vaults
// (where FAR samples are legitimate occluders). Half-res cost scales ~linearly (~1.6x).
static const uint kSteps = 8;      // per side

float3 view_pos(float2 uv, float d)
{
    float4 v = mul(pc.inv_proj, float4(uv * 2.0 - 1.0, d, 1.0));
    return v.xyz / v.w;
}

float depth_at(float2 uv)
{
    return src_textures[pc.depth_slot].SampleLevel(uv, 0).r;
}

// Snap a uv to the center of the full-res depth texel the NEAREST sampler returns for it. The
// half-res dispatch uvs land exactly BETWEEN four full-res texels, so reconstructing a view
// position from the un-snapped xy ray paired with the snapped texel's depth mismatches xy and
// depth by up to half a texel. At grazing angles the per-texel depth gradient is large, so that
// mismatch systematically corrupted every reconstructed position, normal and horizon sample with
// a screen-periodic parity error — banding on flat/smooth surfaces and phantom self-occlusion on
// convex ones. All reconstruction goes through snapped coordinates so xy and depth describe the
// same surface point.
float2 snap_uv(float2 uv)
{
    return (floor(uv * float2(pc.depth_size)) + 0.5) / float2(pc.depth_size);
}

// Interleaved gradient noise: deterministic per-pixel rotation (spatial only — no temporal jitter,
// the denoiser below is purely spatial).
float ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void gtao_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size.x || id.y >= pc.dst_size.y) return;
    // Snap to the exact full-res texel center so the reconstructed xy ray and the NEAREST-sampled
    // depth describe the same point (see snap_uv).
    float2 uv = snap_uv((float2(id.xy) + 0.5) / float2(pc.dst_size));
    float d = depth_at(uv);
    if (d <= 1e-8)   // reverse-Z far plane = sky: fully visible
    {
        dst_images[pc.dst_slot][int2(id.xy)] = float4(0.5, 0.5, 0.5, 1.0);
        return;
    }
    float3 P = view_pos(uv, d);
    float3 V = normalize(-P);

    // Depth-reconstructed view normal (pick the smaller one-sided difference per axis — robust at
    // depth edges). uv is texel-centered, so integer-texel offsets stay texel-centered.
    float2 texel = 1.0 / float2(pc.depth_size);
    float3 pr = view_pos(uv + float2(texel.x, 0), depth_at(uv + float2(texel.x, 0)));
    float3 pl = view_pos(uv - float2(texel.x, 0), depth_at(uv - float2(texel.x, 0)));
    float3 pd = view_pos(uv + float2(0, texel.y), depth_at(uv + float2(0, texel.y)));
    float3 pu = view_pos(uv - float2(0, texel.y), depth_at(uv - float2(0, texel.y)));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, V) < 0.0) N = -N;

    // World radius -> NDC at this depth (per axis; uv offset = ndc * 0.5).
    float2 r_uv = pc.radius * float2(pc.proj00, pc.proj11) / max(-P.z, 1e-3) * 0.5;

    float noise_dir = ign(float2(id.xy));
    float noise_step = frac(noise_dir * 61.803);

    float vis_sum = 0.0;
    float3 bent = float3(0.0);
    for (uint s = 0; s < kSlices; ++s)
    {
        float phi = PI / float(kSlices) * (float(s) + noise_dir);
        float2 dir2 = float2(cos(phi), sin(phi));
        float3 slice_dir = float3(dir2, 0.0);
        float3 axis = normalize(cross(slice_dir, V));
        float3 t_dir = cross(V, axis);

        float3 n_proj = N - axis * dot(N, axis);
        float n_len = length(n_proj);
        if (n_len < 1e-4) continue;
        float3 n_dir = n_proj / n_len;
        float n_cos = clamp(dot(n_dir, V), -1.0, 1.0);
        float gamma = acos(n_cos) * sign(dot(n_dir, t_dir));

        // Horizon search, both sides of the slice.
        float2 cos_h = float2(-1.0, -1.0);
        [unroll] for (int side = 0; side < 2; ++side)
        {
            float sgn = side == 0 ? -1.0 : 1.0;
            // Per-(slice, side) decorrelated step jitter. A single shared step phase + the old
            // f*f center-clustering left the FAR field sampled by a sparse, screen-coherent
            // pattern; on concave receivers (the arcade vaults, where distant samples are
            // LEGITIMATE occluders) the horizon estimate jumped between the sparse far samples
            // as depth varied -> wide dark contour bands that scaled with r.gtao.radius.
            // UNIFORM spacing bounds the largest gap at radius/kSteps, and decorrelating the
            // phase per slice+side turns the residual discretization into per-pixel noise the
            // spatial denoiser can average away.
            float ns = frac(noise_step + (float(s) * 2.0 + float(side)) * 0.618034);
            for (uint st = 0; st < kSteps; ++st)
            {
                float f = (float(st) + ns + 0.5) / float(kSteps);   // uniform spacing (no f*f)
                float2 suv = uv + sgn * dir2 * f * r_uv;
                if (any(suv < 0.0) || any(suv > 1.0)) break;
                suv = snap_uv(suv);   // xy and depth must describe the same texel (see snap_uv)
                float sd = depth_at(suv);
                if (sd <= 1e-8) continue;   // sky sample: no occluder
                float3 SP = view_pos(suv, sd);
                float3 delta = SP - P;
                float dist = length(delta);
                if (dist < 1e-4) continue;
                // Tangent-plane angular bias (the HBAO/XeGTAO bias, ~6 deg): a sample only counts
                // as an occluder if it RISES above P's tangent plane. Coplanar neighbours on flat
                // ground and below-tangent neighbours on convex geometry (sphere tops — curvature
                // is not occlusion) can then never register as horizons; without this, sub-texel
                // reconstruction error made exactly those samples flip in and out of occluding,
                // which banded flat surfaces and darkened smooth spheres.
                if (dot(delta, N) < 0.1 * dist) continue;
                float c = dot(delta / dist, V);
                // Distance falloff: fade the sample out smoothly across the search radius so it
                // reaches full attenuation ("no occluder") by the radius edge; the previous form
                // only reached -1 at 2*radius, which the search (dist <= radius) never sampled.
                float atten = smoothstep(0.0, 1.0, dist / max(pc.radius, 1e-4));
                c = lerp(c, -1.0, atten);
                cos_h[side] = max(cos_h[side], c);
            }
        }
        // Horizon angles about V (side 0 negative), clamped to the hemisphere around the normal.
        float t1 = -acos(clamp(cos_h[0], -1.0, 1.0));
        float t2 = acos(clamp(cos_h[1], -1.0, 1.0));
        t1 = gamma + max(t1 - gamma, -PI * 0.5);
        t2 = gamma + min(t2 - gamma, PI * 0.5);
        // Cosine-weighted visibility of the clamped arc (Jimenez eq. 3).
        // NOTE: not clamped to 1 per slice — the estimator is only exact in the SLICE AVERAGE
        // (oblique unoccluded slices legitimately integrate past 1 and balance the perpendicular
        // ones); the total is saturated below. Clamping here biased flat oblique ground dark.
        float a = 0.25 * (-cos(2.0 * t1 - gamma) + n_cos + 2.0 * t1 * sin(gamma))
                + 0.25 * (-cos(2.0 * t2 - gamma) + n_cos + 2.0 * t2 * sin(gamma));
        vis_sum += n_len * max(a, 0.0);
        // Bent direction: the middle of the unoccluded arc, weighted like the visibility.
        float bent_ang = (t1 + t2) * 0.5;
        bent += (V * cos(bent_ang) + t_dir * sin(bent_ang)) * n_len;
    }
    float vis = saturate(vis_sum / float(kSlices));
    float3 bent_v = normalize(lerp(N, normalize(bent + float3(1e-5, 0, 0)), 0.9));
    // View -> world direction: world = R^T * view (pc.view's upper 3x3 is world->view).
    float3 bent_w = mul(transpose((float3x3)pc.view), bent_v);
    dst_images[pc.dst_slot][int2(id.xy)] = float4(bent_w * 0.5 + 0.5, vis);
}

// Depth-aware 9x9 spatial denoise (single pass at half res; AO is low-frequency).
[shader("compute")]
[numthreads(8, 8, 1)]
void gtao_denoise_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size.x || id.y >= pc.dst_size.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.dst_size);
    float dc = depth_at(uv);
    if (dc <= 1e-8)
    {
        dst_images[pc.dst_slot][int2(id.xy)] =
            src_textures[pc.src_slot].SampleLevel(uv, 0);
        return;
    }
    float zc = view_pos(uv, dc).z;
    float2 texel = 1.0 / float2(pc.dst_size);
    float4 acc = float4(0.0);
    float wsum = 0.0;
    // 9x9 (widened from 5x5, 2026-07-24): averages 81 independent jitter phases instead of 25 —
    // the residual horizon-sampling discretization on the smooth vaults needed more phases to
    // read as flat. AO is low-frequency; the wider kernel costs no visible detail (depth weights
    // still stop bleeding across edges).
    [unroll] for (int y = -4; y <= 4; ++y)
        [unroll] for (int x = -4; x <= 4; ++x)
        {
            float2 suv = uv + float2(x, y) * texel;
            float sd = depth_at(suv);
            if (sd <= 1e-8) continue;
            float sz = view_pos(suv, sd).z;
            float w = exp(-abs(sz - zc) * 4.0);
            acc += src_textures[pc.src_slot].SampleLevel(suv, 0) * w;
            wsum += w;
        }
    float4 result = wsum > 1e-4 ? acc / wsum
                                : src_textures[pc.src_slot].SampleLevel(uv, 0);
    // Renormalize the averaged bent normal.
    float3 bn = result.xyz * 2.0 - 1.0;
    float l = length(bn);
    result.xyz = l > 1e-4 ? (bn / l) * 0.5 + 0.5 : float3(0.5, 0.5, 0.5);
    dst_images[pc.dst_slot][int2(id.xy)] = result;
}
