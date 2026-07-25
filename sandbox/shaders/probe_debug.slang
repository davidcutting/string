// Brief 09b — instanced probe-debug spheres. One small sphere per probe, GPU-driven (no vertex
// buffer): SV_VertexID indexes a procedural UV-sphere, SV_InstanceID selects the probe. Each sphere
// is shaded with the probe's own stored irradiance in the surface-normal direction (mode 0) or its
// visibility mean distance as greyscale (mode 1) — the M0/M1/M2 acceptance view. Depth-tested so it
// sits in the scene. The atlas is sampled directly on the GPU (no readback).
import probe_common;

static const float DBG_PI = 3.14159265359;

struct Push
{
    float4x4 view_proj;
    float4 origin_spacing;   // xyz origin, w = sphere radius
    float4 spacing;          // xyz per-axis spacing
    uint4 counts;            // xyz probe counts, w = mode (1 flat grey, 2 irradiance, 3 visibility)
    float4 camera_pos;       // xyz, w = exposure scale
    uint irrad_slot;
    uint vis_slot;
    uint _pad0;
    uint _pad1;
    ProbeActiveBuffer *active;   // per-probe activation (1 active / 0 inside geometry)
    ProbeOffsetBuffer *offset;   // per-probe relocation offset (RTXGI) — moves the sphere out of walls
};
[[vk::push_constant]] Push pc;

[[vk::binding(1, 0)]] Sampler2D lit_textures[];

// UV-sphere tessellation. rings x sectors quads -> 2 tris each. Keep small (debug only).
static const uint kRings = 8;
static const uint kSectors = 12;
static const uint kVertsPerSphere = kRings * kSectors * 6;

// SV_StartVertexLocation / SV_StartInstanceLocation restore the D3D base-subtracted semantics (see
// the standing Slang gotchas): the draw uses firstInstance/vertexOffset 0 here, but declaring them
// keeps parity with the convention used across this codebase.
struct VsIn
{
    uint vid : SV_VertexID;
    uint iid : SV_InstanceID;
    uint base_v : SV_StartVertexLocation;
    uint base_i : SV_StartInstanceLocation;
};

struct VsOut
{
    float4 pos : SV_Position;
    float3 normal : NORMAL0;
    nointerpolation uint probe : PROBE0;
};

float3 sphere_vertex(uint vid, out float3 normal)
{
    // Which quad + which corner.
    uint quad = vid / 6;
    uint corner = vid % 6;
    uint ring = quad / kSectors;
    uint sector = quad % kSectors;
    // The two triangles of the quad: (0,1,2) and (0,2,3) with corners tl,tr,br,bl.
    uint2 ci[6] = { uint2(0,0), uint2(1,0), uint2(1,1), uint2(0,0), uint2(1,1), uint2(0,1) };
    uint2 off = ci[corner];
    float u = float(sector + off.x) / float(kSectors);
    float v = float(ring + off.y) / float(kRings);
    float theta = v * DBG_PI;             // 0..pi (pole to pole)
    float phi = u * 2.0 * DBG_PI;         // 0..2pi
    float st = sin(theta);
    normal = float3(st * cos(phi), cos(theta), st * sin(phi));
    return normal;
}

// Rebuild the probe coord from linear index using the push counts.
uint3 probe_coord_from_counts(uint linear)
{
    uint x = linear % pc.counts.x;
    uint y = (linear / pc.counts.x) % pc.counts.y;
    uint z = linear / (pc.counts.x * pc.counts.y);
    return uint3(x, y, z);
}

[shader("vertex")]
VsOut vs_main(VsIn in)
{
    VsOut o;
    float3 n;
    float3 local = sphere_vertex(in.vid, n);
    uint3 c = probe_coord_from_counts(in.iid);
    // Effective probe position = grid position + relocation offset (RTXGI) so the debug sphere sits
    // where the probe actually samples from — visibly nudged out of walls/geometry.
    float3 center = pc.origin_spacing.xyz + float3(c) * pc.spacing.xyz + pc.offset.offset[in.iid].xyz;
    // Hide probes classified INACTIVE (inside geometry): collapse the sphere to a degenerate point so
    // the debug view shows only the probes that actually contribute — visual confirmation of the
    // inside-geometry classification the user flagged.
    float radius = pc.active.state[in.iid] != 0u ? pc.origin_spacing.w : 0.0;
    float3 world = center + local * radius;
    o.pos = mul(pc.view_proj, float4(world, 1.0));
    o.normal = n;
    o.probe = in.iid;
    return o;
}

ProbeGrid make_grid()
{
    ProbeGrid g;
    g.origin = pc.origin_spacing.xyz;
    g.spacing = pc.spacing.xyz;
    g.counts = pc.counts.xyz;
    g.irrad_stride = 10;
    g.vis_stride = 18;
    g.offsets = pc.offset;
    g.active = pc.active;
    return g;
}

[shader("fragment")]
float4 fs_main(VsOut i) : SV_Target
{
    ProbeGrid g = make_grid();
    uint3 c = probe_coord_from_counts(i.probe);
    float3 n = normalize(i.normal);
    // Simple lambert against a fixed key so the sphere reads as a solid 3D ball, not a flat disc.
    // High ambient floor so even away-facing texels stay a readable light grey (debug aid, not lit).
    float ndl = saturate(dot(n, normalize(float3(0.4, 0.8, 0.5)))) * 0.35 + 0.65;

    // These spheres draw into the scene HDR target and then go through the composite pass's EV100
    // exposure + ACES like everything else. Dividing by the exposure scale (camera_pos.w) makes the
    // on-screen brightness exposure-independent: composite multiplies it back out, so `target` is the
    // pre-tonemap linear value we actually want.
    float inv_exp = 1.0 / max(pc.camera_pos.w, 1e-6);

    if (pc.counts.w == 1u)
    {
        // Flat-grey mode (M0 acceptance / placement check): every probe a neutral grey ball, so the
        // grid layout + instancing + depth are verifiable independent of the atlas contents.
        return float4(float3(0.7) * ndl * inv_exp, 1.0);
    }

    if (pc.counts.w == 3u)
    {
        // Visibility mode: mean distance as greyscale, scaled by the cell diagonal so it reads.
        // Distance isn't HDR radiance, so map it to a fixed grey and undo the composite exposure.
        float2 vuv = probe_tile_uv(g, c, 18, 16, n);
        float mean = lit_textures[pc.vis_slot].SampleLevel(vuv, 0).r;
        float cell = length(pc.spacing.xyz);
        float t = saturate(mean / max(cell * 2.0, 1e-3));
        return float4(float3(t) * ndl * inv_exp, 1.0);
    }
    // Irradiance mode: the probe's stored E/pi in the surface-normal direction (the lit view). This
    // is genuine HDR radiance in the scene's units, so output it RAW — the composite pass applies
    // the same exposure + ACES it applies to the rest of the scene (do NOT pre-expose here).
    float2 iuv = probe_tile_uv(g, c, 10, 8, n);
    float3 e = lit_textures[pc.irrad_slot].SampleLevel(iuv, 0).rgb;   // E/pi (HDR knits)
    return float4(e, 1.0);
}
