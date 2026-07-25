// Brief 09b — probe capture (STATIC, load-time). Rasterizes a small per-probe G-buffer cubemap of
// the static scene, then reprojects it into the probe's octahedral capture texels (albedo, world
// normal, hit distance | sky-miss) and the DDGI visibility atlas (mean / mean^2 of distance). This
// never changes after load; the relight compute reads these to recompute radiance under the live sun.
//
// M0 STATUS: capture rasterization is not wired yet. `clear_main` initializes the atlases to a
// "sky-miss / fully-visible" state so the M0 debug spheres and the flat-sky relight are well-defined.
// M1 replaces this with the real cubemap-reprojection pass (raster path + octahedral collapse).
import probe_common;

[[vk::binding(2, 0)]] RWTexture2D<float4> images_2d[];   // storage views (capture / vis atlases)
[[vk::binding(1, 0)]] SamplerCube cap_cubes[];           // cube G-buffer faces (albedo / normal+dist)

struct ActiveBuffer { uint state[]; };   // per-probe activation: 1 = active, 0 = inside geometry
struct OffsetBuffer { float4 offset[]; };  // per-probe relocation offset (xyz world, w unused)

struct Push
{
    uint probe_base;      // first probe index this dispatch covers
    uint probe_count;     // probes in this dispatch (amortized capture)
    uint counts_x;
    uint counts_y;
    uint counts_z;
    uint cap_gbuf_slot;   // storage slot: capture normal.xyz + hit distance (w<0 => sky miss)
    uint cap_albedo_slot; // storage slot: capture albedo.rgb
    uint vis_slot;        // storage slot: visibility mean / mean^2
    float far_distance;   // "no occluder" distance written to visibility (>= volume diagonal)
    // --- M1 collapse fields (unused by clear_main) ---
    uint cube_albedo_slot; // SamplerCube slot: this probe's captured albedo faces
    uint cube_nd_slot;     // SamplerCube slot: this probe's captured normal.xyz + linear dist (w)
    uint round2;           // bake round: 0 = compute relocation, 1 = re-capture from relocated pos
                           // (offset is NOT rewritten in round 2 — no drift; classification re-votes)
    float3 probe_pos;      // this probe's world position (collapse operates on ONE probe)
    uint probe_index;      // this probe's linear index (collapse: one probe per dispatch)
    ActiveBuffer *active;  // per-probe activation state (classification output)
    float3 spacing;        // per-axis grid spacing (relocation clamp = +-0.5*spacing per axis)
    OffsetBuffer *offset;  // per-probe relocation offset (classification/relocation output)
};
[[vk::push_constant]] Push pc;

uint3 coord_of(uint linear)
{
    uint x = linear % pc.counts_x;
    uint y = (linear / pc.counts_x) % pc.counts_y;
    uint z = linear / (pc.counts_x * pc.counts_y);
    return uint3(x, y, z);
}

ProbeGrid make_grid()
{
    ProbeGrid g;
    g.origin = float3(0.0);
    g.spacing = float3(1.0);
    g.counts = uint3(pc.counts_x, pc.counts_y, pc.counts_z);
    g.irrad_stride = 10;
    g.vis_stride = 18;
    g.offsets = nullptr;   // collapse writes offsets; it never reads probe_world.
    g.active = nullptr;
    return g;
}

// M0 initializer: mark every capture texel a sky-miss and every visibility texel "far" (unoccluded),
// including the gutter, so bilinear reads at tile edges stay well-defined. One thread per texel of
// the 18x18 vis/capture tile, per probe in the dispatch.
[shader("compute")]
[numthreads(8, 8, 1)]
void clear_main(uint3 id : SV_DispatchThreadID)
{
    uint stride = 18;
    if (id.x >= stride || id.y >= stride) return;
    uint probe = pc.probe_base + id.z;
    if (id.z >= pc.probe_count) return;
    ProbeGrid g = make_grid();
    uint3 c = coord_of(probe);
    uint col = c.y * pc.counts_x + c.x;
    uint row = c.z;
    int2 texel = int2(col * stride, row * stride) + int2(id.xy);

    // Sky-miss capture (albedo 0, normal 0, distance < 0 flags miss).
    images_2d[pc.cap_gbuf_slot][texel] = float4(0.0, 0.0, 0.0, -1.0);
    images_2d[pc.cap_albedo_slot][texel] = float4(0.0, 0.0, 0.0, 1.0);
    // Visibility: mean = far, mean^2 = far^2 (variance 0 -> Chebyshev never rejects).
    float f = pc.far_distance;
    images_2d[pc.vis_slot][texel] = float4(f, f * f, 0.0, 0.0);
}

// --- M1 collapse: reproject ONE probe's cube G-buffer into its octahedral capture + vis atlases ---
// One workgroup per probe, 18x18 threads (the vis/capture tile incl. the 1-texel gutter). Each texel
// decodes its octahedral direction, samples the captured cube in that direction, and writes {albedo,
// packed normal, hit distance | sky-miss}. Distance accumulates into the DDGI visibility mean/mean^2.
// The gutter texels reflect across the octahedral seam (Cigolle/Majercik border rule). During the
// pass each thread votes on backface / near hits; the workgroup reduces the fraction to CLASSIFY the
// probe as inside geometry (INACTIVE) — the standard DDGI probe-state fix.
groupshared uint gs_backface;   // count of directions hitting a backface or a very-near occluder
groupshared uint gs_total;      // count of directions that hit anything (not a sky miss)
// Relocation accumulator (RTXGI-style): sum of push-away vectors from near hits (fixed-point x1e4 so
// InterlockedAdd works on ints), plus a near-hit count. The probe nudges away from the closest
// geometry into open space, clamped to its cell so it stays associated with the grid position.
groupshared int gs_push_x;
groupshared int gs_push_y;
groupshared int gs_push_z;
groupshared uint gs_near;

[shader("compute")]
[numthreads(18, 18, 1)]   // kProbeVisStride
void collapse_main(uint3 id : SV_DispatchThreadID, uint3 lid : SV_GroupThreadID)
{
    uint stride = 18;      // kProbeVisStride
    uint inner = 16;       // kProbeVisSize
    if (lid.x == 0 && lid.y == 0)
    {
        gs_backface = 0; gs_total = 0;
        gs_push_x = 0; gs_push_y = 0; gs_push_z = 0; gs_near = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    ProbeGrid g = make_grid();
    g.counts = uint3(pc.counts_x, pc.counts_y, pc.counts_z);
    uint3 c = coord_of(pc.probe_index);
    uint col = c.y * pc.counts_x + c.x;
    uint row = c.z;
    int2 tile_origin = int2(col * stride, row * stride);
    int2 local = int2(lid.xy);            // 0..17 within the tile (incl. gutter)
    int2 inner_xy = local - int2(1, 1);   // -1..16

    // Border reflection across the octahedral seam (same rule as probe_relight.slang).
    int2 m = inner_xy;
    bool2 lo = inner_xy < int2(0);
    bool2 hi = inner_xy >= int2(inner);
    if (lo.x) { m.x = 0; m.y = inner - 1 - m.y; }
    if (hi.x) { m.x = inner - 1; m.y = inner - 1 - m.y; }
    if (lo.y) { m.y = 0; m.x = inner - 1 - m.x; }
    if (hi.y) { m.y = inner - 1; m.x = inner - 1 - m.x; }
    m = clamp(m, int2(0), int2(inner - 1));
    float2 oct = ((float2(m) + 0.5) / float(inner)) * 2.0 - 1.0;   // [-1,1]
    float3 dir = oct_decode(oct);

    // Sample the captured cube in this direction. nd.w < 0 (clear value) => sky miss.
    float4 nd = cap_cubes[pc.cube_nd_slot].SampleLevel(dir, 0);
    float4 alb = cap_cubes[pc.cube_albedo_slot].SampleLevel(dir, 0);
    // Bilinear fp16 cube read: a texel straddling a silhouette blends a covered normal (unit, w=+d)
    // against the sky-miss clear (0,0,0,-1). Where that lands a "hit" (w>=0) on a cancelled normal,
    // normalize() is non-finite. Treat a degenerate normal as a sky miss so no NaN is ever written
    // into the capture G-buffer / visibility atlas.
    float nlen = length(nd.xyz);
    bool miss = nd.w < 0.0 || nlen < 1e-4;
    float3 hit_n = miss ? float3(0.0) : nd.xyz / nlen;
    float dist = miss ? pc.far_distance : nd.w;

    // Classification vote (inner texels only; gutter is a duplicate). A backface hit means the probe
    // sees the INSIDE of geometry (dir . hit_normal > 0); a very-near hit also implies it is buried.
    bool is_inner = !any(lo) && !any(hi);
    // A hit is "near" if it is closer than half the smallest cell dimension — geometry that close
    // means the probe is buried or hugging a wall in this direction. Relocation pushes away from it.
    float near_thresh = 0.5 * min(pc.spacing.x, min(pc.spacing.y, pc.spacing.z));
    if (is_inner)
    {
        InterlockedAdd(gs_total, 1u);
        bool backface = !miss && dot(dir, hit_n) > 0.0;
        bool very_near = !miss && dist < 0.05;
        if (backface || very_near) InterlockedAdd(gs_backface, 1u);
        // Relocation vote: for a near hit, accumulate a push-away vector (opposite the hit dir),
        // weighted by how near it is (closer => stronger push). Backfaces push hardest (we are inside).
        if (!miss && (backface || dist < near_thresh))
        {
            float w = backface ? 1.0 : saturate(1.0 - dist / max(near_thresh, 1e-3));
            float3 push = -dir * w;
            InterlockedAdd(gs_push_x, int(push.x * 10000.0));
            InterlockedAdd(gs_push_y, int(push.y * 10000.0));
            InterlockedAdd(gs_push_z, int(push.z * 10000.0));
            InterlockedAdd(gs_near, 1u);
        }
    }

    // Capture atlas writes (albedo + normal/dist), including the gutter.
    int2 texel = tile_origin + local;
    images_2d[pc.cap_gbuf_slot][texel] = float4(hit_n, miss ? -1.0 : dist);
    images_2d[pc.cap_albedo_slot][texel] = float4(miss ? float3(0.0) : alb.rgb, 1.0);
    // Visibility mean / mean^2 of the hit distance. DDGI convention (main-agent tip 5): a BACKFACE
    // hit (the probe sees the inside of a wall in this direction) records a deliberately SHORTENED
    // distance (x0.2). That collapses the Chebyshev mean toward the wall so any shading point on the
    // FAR side is aggressively rejected — the second line of defense that makes imperfect probe
    // placement tolerable (dead/near-dead probes stop leaking through walls). Sky miss = far
    // (open sky reads as unoccluded).
    bool vis_backface = !miss && dot(dir, hit_n) > 0.0;
    float vis_dist = vis_backface ? dist * 0.2 : dist;
    images_2d[pc.vis_slot][texel] = float4(vis_dist, vis_dist * vis_dist, 0.0, 0.0);

    // Reduce the classification + relocation and write the probe's state (thread 0).
    GroupMemoryBarrierWithGroupSync();
    if (lid.x == 0 && lid.y == 0)
    {
        // Relocation (RTXGI-style): the mean push-away vector, clamped to +-0.5*spacing per axis so
        // the probe stays inside its own grid cell. Applied consistently everywhere probe_world() is
        // used (relight bounce, shading, debug) via probe_common. A probe still buried after the
        // clamp is left INACTIVE by the classification below.
        float3 push = float3(0.0);
        if (gs_near > 0u)
        {
            push = float3(float(gs_push_x), float(gs_push_y), float(gs_push_z))
                 / (10000.0 * float(gs_near));
        }
        float3 limit = 0.5 * pc.spacing;
        float3 off = clamp(push * (0.5 * min(pc.spacing.x, min(pc.spacing.y, pc.spacing.z))),
                           -limit, limit);
        // Round 1 only: round 2 re-captures FROM the relocated position, so rewriting the offset
        // from that view would compound relocations and de-sync the (just-captured) distances.
        if (pc.round2 == 0u)
            pc.offset.offset[pc.probe_index] = float4(off, 0.0);

        // Inside geometry if a large fraction of directions see a backface / near occluder. RTXGI
        // classifies inside probes at ~0.25 backface ratio; we use 0.45 — aggressive enough to
        // deactivate a probe embedded in a wall or column after relocation fails (its atlas texels
        // would otherwise darken every cell that touches it), but high enough NOT to evict a probe
        // that merely hugs a wall: evicting those makes an interior point fall back to a farther
        // EXTERIOR probe, which bleeds bright light through thin shells/corners (the leak artifact).
        // Relocation above is the first line; the grid now insets inside the AABB too.
        const float kInsideBackfaceRatio = 0.45;
        float ratio = gs_total > 0u ? float(gs_backface) / float(gs_total) : 0.0;
        pc.active.state[pc.probe_index] = ratio > kInsideBackfaceRatio ? 0u : 1u;
    }
}
