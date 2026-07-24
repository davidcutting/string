// Dynamic sky image-based lighting (brief 07): split-sum IBL baked at RUNTIME from the live
// procedural sky, entirely on the GPU. Five compute entry points, all driven by one Push:
//
//   capture_main   sky_env() -> env capture cubemap mip 0 (no sun disc: the analytic sun is the
//                  direct light; a furnace flag writes a uniform white environment instead)
//   mip_main       2x2 box reduce of the capture chain (mip N -> N+1), the prefilter's PDF-mip
//                  source and the SH projection source
//   prefilter_main GGX-importance-sampled roughness ladder (one dispatch per output mip of the
//                  prefiltered cubemap; perceptual roughness = mip / (mips-1), alpha = r^2)
//   sh_project_main one-workgroup L2 SH projection (9 RGB coefficients) of a small capture mip,
//                  cosine-convolved and pre-divided by pi so the lit shader's reconstruction is
//                  E/pi (a uniform white environment reconstructs exactly 1.0 — the furnace DC)
//   dfg_main       split-sum BRDF integration LUT: x = scale (integral without Fresnel), y = bias
//                  (Fresnel-weighted). Height-correlated Smith visibility — the SAME visibility
//                  the direct BRDF uses, so Kulla-Conty energy compensation
//                  (1 + F0*(1/(x+y) - 1)) and Fdez-Aguera multi-scatter read straight off it.
//
// Roughness convention (audited, brief 07 M1): glTF PERCEPTUAL roughness r everywhere at the
// interface; alpha = r*r inside D/V/importance sampling. The prefilter ladder and the DFG LUT both
// index by PERCEPTUAL r, matching the lit shader's lookups.
import sky;

static const float PI = 3.14159265359;

[[vk::binding(1, 0)]] SamplerCube env_cubes[];             // capture chain (sampled by the prefilter)
[[vk::binding(2, 0)]] RWTexture2DArray<float4> cube_faces[]; // per-mip 6-layer storage views
[[vk::binding(2, 0)]] RWTexture2D<float4> images_2d[];       // DFG LUT storage view (aliased binding)

struct ShBuffer { float4 c[]; };   // 9 live entries (unsized: see the note in lighting.slang)

struct Push
{
    float4 sun_dir;      // xyz sun dir; w = furnace flag (1 -> capture uniform white)
    float4 sky_zenith;   // xyz
    float4 sky_ground;   // xyz ground ALBEDO (radiance is derived in sky_ground_radiance)
    float4 sun_color;    // xyz; w = sun intensity (klx perpendicular)
    uint src_slot;       // capture: unused; mip: src storage mip; prefilter: capture SamplerCube slot; sh: storage mip
    uint dst_slot;       // storage view being written (cube mip or the DFG LUT)
    uint dst_size;       // face/LUT size of the destination (square)
    uint src_size;       // face size of the source mip (mip/sh) / capture base size (prefilter)
    float roughness;     // prefilter: PERCEPTUAL roughness of this ladder step
    uint sample_count;   // prefilter/dfg sample count
    uint mip_count;      // prefilter: capture chain mip count (PDF lod clamp)
    uint _pad0;
    ShBuffer *sh;        // sh_project: output coefficients
};
[[vk::push_constant]] Push pc;

// Texel (face, x, y) -> outward direction, standard Vulkan/D3D cube face layout. Must match the
// hardware SamplerCube convention: writes go through this mapping, reads through the sampler.
float3 face_dir(uint face, float2 uv)   // uv in [-1, 1]
{
    switch (face)
    {
        case 0:  return float3( 1.0, -uv.y, -uv.x);   // +X
        case 1:  return float3(-1.0, -uv.y,  uv.x);   // -X
        case 2:  return float3( uv.x,  1.0,  uv.y);   // +Y
        case 3:  return float3( uv.x, -1.0, -uv.y);   // -Y
        case 4:  return float3( uv.x, -uv.y,  1.0);   // +Z
        default: return float3(-uv.x, -uv.y, -1.0);   // -Z
    }
}

float2 texel_uv(uint2 xy, uint size)
{
    return (float2(xy) + 0.5) / float(size) * 2.0 - 1.0;
}

// --- Sky -> cubemap capture ---------------------------------------------------------------------
[shader("compute")]
[numthreads(8, 8, 1)]
void capture_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size || id.y >= pc.dst_size) return;
    float3 dir = normalize(face_dir(id.z, texel_uv(id.xy, pc.dst_size)));
    float3 radiance = pc.sun_dir.w > 0.5
        ? float3(1.0)   // furnace: uniform white environment
        : sky_env(dir, pc.sun_dir.xyz, pc.sky_zenith.xyz, pc.sky_ground.xyz, pc.sun_color.xyz,
                  pc.sun_color.w);
    cube_faces[pc.dst_slot][int3(id.xy, id.z)] = float4(radiance, 1.0);
}

// --- Capture mip chain (2x2 box, per face; no cross-face filtering — PDF-mip source only) --------
[shader("compute")]
[numthreads(8, 8, 1)]
void mip_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size || id.y >= pc.dst_size) return;
    int2 s = int2(id.xy) * 2;
    int2 mx = int2(pc.src_size - 1, pc.src_size - 1);
    float4 c = cube_faces[pc.src_slot][int3(min(s + int2(0, 0), mx), id.z)]
             + cube_faces[pc.src_slot][int3(min(s + int2(1, 0), mx), id.z)]
             + cube_faces[pc.src_slot][int3(min(s + int2(0, 1), mx), id.z)]
             + cube_faces[pc.src_slot][int3(min(s + int2(1, 1), mx), id.z)];
    cube_faces[pc.dst_slot][int3(id.xy, id.z)] = c * 0.25;
}

// --- Shared sampling helpers ---------------------------------------------------------------------
float2 hammersley(uint i, uint count)
{
    uint bits = reversebits(i);
    return float2(float(i) / float(count), float(bits) * 2.3283064365386963e-10);
}

// GGX half-vector importance sample about +Z. alpha = perceptual_roughness^2.
float3 importance_sample_ggx(float2 xi, float alpha)
{
    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
    return float3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
}

float d_ggx(float NdotH, float alpha)
{
    float a2 = alpha * alpha;
    float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * dd * dd, 1e-8);
}

// Height-correlated Smith visibility (Heitz 2014). V = G2 / (4 NdotV NdotL).
float v_smith_ggx_correlated(float NdotV, float NdotL, float alpha)
{
    float a2 = alpha * alpha;
    float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-7);
}

// --- GGX prefilter ladder -------------------------------------------------------------------------
[shader("compute")]
[numthreads(8, 8, 1)]
void prefilter_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size || id.y >= pc.dst_size) return;
    float3 N = normalize(face_dir(id.z, texel_uv(id.xy, pc.dst_size)));

    if (pc.roughness <= 0.0)
    {
        // Roughness 0 = mirror: the capture itself.
        float3 c = env_cubes[pc.src_slot].SampleLevel(N, 0).rgb;
        cube_faces[pc.dst_slot][int3(id.xy, id.z)] = float4(c, 1.0);
        return;
    }

    float alpha = pc.roughness * pc.roughness;
    // Split-sum assumption: N = V = R.
    float3 V = N;
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    // PDF-based mip selection vs the capture chain's texel solid angle (kills fireflies from the
    // glow without giant sample counts).
    float sa_texel = 4.0 * PI / (6.0 * float(pc.src_size) * float(pc.src_size));

    float3 sum = float3(0.0);
    float weight = 0.0;
    for (uint i = 0; i < pc.sample_count; ++i)
    {
        float3 h = importance_sample_ggx(hammersley(i, pc.sample_count), alpha);
        float3 H = T * h.x + B * h.y + N * h.z;
        float3 L = 2.0 * dot(V, H) * H - V;
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 1e-4);
        float pdf = d_ggx(NdotH, alpha) * NdotH / (4.0 * VdotH);
        float sa_sample = 1.0 / max(float(pc.sample_count) * pdf, 1e-7);
        float lod = clamp(0.5 * log2(sa_sample / sa_texel), 0.0, float(pc.mip_count - 1));
        sum += env_cubes[pc.src_slot].SampleLevel(L, lod).rgb * NdotL;
        weight += NdotL;
    }
    float3 c = sum / max(weight, 1e-6);
    cube_faces[pc.dst_slot][int3(id.xy, id.z)] = float4(c, 1.0);
}

// --- L2 SH projection (one workgroup) -------------------------------------------------------------
// Reads a small capture mip (src_size x src_size x 6, storage view src_slot), projects onto the 9
// SH basis functions with per-texel solid-angle weights, cosine-convolves (A0=pi, A1=2pi/3,
// A2=pi/4) and divides by pi. The lit shader's dot-product reconstruction then yields E/pi
// directly: a uniform environment of radiance 1 gives exactly 1 for every normal.
static const uint kShThreads = 256;
groupshared float3 sh_partial[kShThreads];

[shader("compute")]
[numthreads(kShThreads, 1, 1)]
void sh_project_main(uint tid : SV_GroupThreadID)
{
    float3 acc[9];
    [unroll] for (uint k = 0; k < 9; ++k) acc[k] = float3(0.0);

    uint total = pc.src_size * pc.src_size * 6;
    for (uint t = tid; t < total; t += kShThreads)
    {
        uint face = t / (pc.src_size * pc.src_size);
        uint rem = t % (pc.src_size * pc.src_size);
        uint2 xy = uint2(rem % pc.src_size, rem / pc.src_size);
        float2 uv = texel_uv(xy, pc.src_size);
        float3 d = face_dir(face, uv);
        // Cube texel solid angle: dA = (2/N)^2 on the face plane, projected by (u^2+v^2+1)^-3/2.
        float len2 = dot(d, d);   // u^2 + v^2 + 1 (face axis is unit)
        float dw = 4.0 / (float(pc.src_size) * float(pc.src_size) * len2 * sqrt(len2));
        float3 n = d / sqrt(len2);
        float3 L = cube_faces[pc.src_slot][int3(xy, face)].rgb * dw;

        acc[0] += L * 0.282095;
        acc[1] += L * (0.488603 * n.y);
        acc[2] += L * (0.488603 * n.z);
        acc[3] += L * (0.488603 * n.x);
        acc[4] += L * (1.092548 * n.x * n.y);
        acc[5] += L * (1.092548 * n.y * n.z);
        acc[6] += L * (0.315392 * (3.0 * n.z * n.z - 1.0));
        acc[7] += L * (1.092548 * n.x * n.z);
        acc[8] += L * (0.546274 * (n.x * n.x - n.y * n.y));
    }

    // Cosine-lobe convolution / pi: l=0 -> 1, l=1 -> 2/3, l=2 -> 1/4.
    static const float a_hat[9] = { 1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0,
                                    0.25, 0.25, 0.25, 0.25, 0.25 };
    for (uint k = 0; k < 9; ++k)
    {
        sh_partial[tid] = acc[k];
        GroupMemoryBarrierWithGroupSync();
        for (uint stride = kShThreads / 2; stride > 0; stride >>= 1)
        {
            if (tid < stride) sh_partial[tid] += sh_partial[tid + stride];
            GroupMemoryBarrierWithGroupSync();
        }
        if (tid == 0) pc.sh.c[k] = float4(sh_partial[0] * a_hat[k], 0.0);
        GroupMemoryBarrierWithGroupSync();
    }
}

// --- Split-sum DFG LUT -----------------------------------------------------------------------------
// x-axis: NdotV in (0,1]; y-axis: PERCEPTUAL roughness in (0,1]. Output: (scale, bias) such that
// the single-scatter environment BRDF is F0*scale + f90*bias, and the directional albedo for
// F0 = 1 is scale + bias (the Kulla-Conty / Fdez-Aguera energy term).
[shader("compute")]
[numthreads(8, 8, 1)]
void dfg_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_size || id.y >= pc.dst_size) return;
    float NdotV = (float(id.x) + 0.5) / float(pc.dst_size);
    float perceptual = (float(id.y) + 0.5) / float(pc.dst_size);
    float alpha = perceptual * perceptual;

    float3 V = float3(sqrt(max(1.0 - NdotV * NdotV, 0.0)), 0.0, NdotV);
    float a = 0.0, b = 0.0;
    for (uint i = 0; i < pc.sample_count; ++i)
    {
        float3 H = importance_sample_ggx(hammersley(i, pc.sample_count), alpha);
        float3 L = 2.0 * dot(V, H) * H - V;
        float NdotL = L.z;
        if (NdotL <= 0.0) continue;
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 1e-4);
        // Importance-sampled estimator: BRDF/pdf * NdotL = 4 * Vis * VdotH * NdotL / NdotH.
        float vis = v_smith_ggx_correlated(NdotV, NdotL, alpha);
        float g_vis = 4.0 * vis * VdotH * NdotL / max(NdotH, 1e-4);
        float fc = pow(1.0 - VdotH, 5.0);
        a += (1.0 - fc) * g_vis;
        b += fc * g_vis;
    }
    images_2d[pc.dst_slot][int2(id.xy)] =
        float4(a / float(pc.sample_count), b / float(pc.sample_count), 0.0, 0.0);
}
