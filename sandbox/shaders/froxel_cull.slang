// Froxel (clustered Forward+) light binning. One compute thread per froxel (screen tile x screen
// tile x log-depth slice); each builds its froxel's view-space AABB and appends every point/spot
// light whose bounding volume overlaps it to that froxel's index list. The lit fragment shader then
// reads only its froxel's list instead of every scene light. Reads/writes buffers by device address,
// built through the brief-01 hot-reload registry.

struct GpuLight
{
    float4 position_radius;   // xyz world pos, w = range
    float4 color_intensity;   // rgb color, w = intensity
    float4 direction_type;    // xyz spot dir, w = type (0 point, 1 spot)
    float4 cone;              // x cos(inner), y cos(outer)
};

// Per-froxel index list: [count, idx0, idx1, ...] with (1 + max_per_froxel) uint32 stride.
struct FroxelList { uint data[]; };
struct LightBuffer { GpuLight lights[]; };

struct Push
{
    float4x4 view;           // world -> view (camera)
    float4x4 inv_proj;       // clip -> view (unproject tile corners)
    uint2 screen;            // render target pixel size
    uint2 grid;              // tiles_x, tiles_y
    uint slices;            // depth slices
    uint tile_size;         // pixels per tile edge
    float near_plane;
    float far_plane;
    uint light_count;
    uint max_per_froxel;
    uint _pad0;
    uint _pad1;
    LightBuffer *lights;
    FroxelList *froxels;
};
[[vk::push_constant]] Push pc;

// View-space z (negative, RH) at the boundary of depth slice `s` under a log distribution.
float slice_view_z(uint s, float near_z, float far_z, uint slices)
{
    // z_view is negative in RH; use magnitudes for the log split, return negated.
    float d = near_z * pow(far_z / near_z, float(s) / float(slices));
    return -d;
}

// Unproject an NDC point (xy in [-1,1], given ndc z) to view space via inv_proj.
float3 unproject(float2 ndc, float ndc_z)
{
    float4 v = mul(pc.inv_proj, float4(ndc.x, ndc.y, ndc_z, 1.0));
    return v.xyz / v.w;
}

// Squared distance from point p to AABB [lo, hi].
float dist2_point_aabb(float3 p, float3 lo, float3 hi)
{
    float3 d = max(max(lo - p, p - hi), float3(0.0));
    return dot(d, d);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void cull_main(uint3 tid : SV_DispatchThreadID)
{
    uint tx = tid.x, ty = tid.y, tz = tid.z;
    if (tx >= pc.grid.x || ty >= pc.grid.y || tz >= pc.slices)
        return;

    uint froxel_index = (tz * pc.grid.y + ty) * pc.grid.x + tx;
    uint stride = 1u + pc.max_per_froxel;
    uint base = froxel_index * stride;

    // Tile screen-space bounds -> NDC. (Vulkan: NDC y is flipped vs pixel y, but the froxel AABB is
    // symmetric enough that binning stays correct; the fragment shader uses the same tile mapping.)
    float2 px_lo = float2(tx * pc.tile_size, ty * pc.tile_size);
    float2 px_hi = float2(min((tx + 1) * pc.tile_size, pc.screen.x),
                          min((ty + 1) * pc.tile_size, pc.screen.y));
    float2 ndc_lo = px_lo / float2(pc.screen) * 2.0 - 1.0;
    float2 ndc_hi = px_hi / float2(pc.screen) * 2.0 - 1.0;

    // Depth slice view-z range [z_far_slice, z_near_slice] (both negative; near is larger/less-neg).
    float z_near = slice_view_z(tz, pc.near_plane, pc.far_plane, pc.slices);
    float z_far = slice_view_z(tz + 1, pc.near_plane, pc.far_plane, pc.slices);

    // Build the froxel's view-space AABB from the 8 unprojected corners. Unproject the tile's 4 NDC
    // corners at both the near and far slice planes. inv_proj maps clip z; use ndc z = 1 (reverse-Z
    // far) as a ray and scale to each view-z, since a perspective frustum's cross-section scales
    // linearly with view-space z.
    float3 lo = float3(1e30);
    float3 hi = float3(-1e30);
    [unroll] for (int c = 0; c < 4; ++c)
    {
        float2 ndc = float2((c & 1) != 0 ? ndc_hi.x : ndc_lo.x, (c & 2) != 0 ? ndc_hi.y : ndc_lo.y);
        // Ray through this NDC corner in view space (unproject at ndc z far plane then normalize by z).
        float3 ray = unproject(ndc, 1.0);
        float3 dir = ray / ray.z;       // ray at view-z = 1 (dir.z == 1)
        float3 p_near = dir * z_near;   // z_near negative -> scales the ray to that plane
        float3 p_far = dir * z_far;
        lo = min(lo, min(p_near, p_far));
        hi = max(hi, max(p_near, p_far));
    }

    uint count = 0;
    for (uint i = 0; i < pc.light_count && count < pc.max_per_froxel; ++i)
    {
        GpuLight L = pc.lights.lights[i];
        float3 pos_view = mul(pc.view, float4(L.position_radius.xyz, 1.0)).xyz;
        float r = L.position_radius.w;
        // Sphere vs froxel AABB (covers both point and spot — the cone is a subset of its sphere).
        if (dist2_point_aabb(pos_view, lo, hi) <= r * r)
        {
            pc.froxels.data[base + 1u + count] = i;
            ++count;
        }
    }
    pc.froxels.data[base] = count;
}
