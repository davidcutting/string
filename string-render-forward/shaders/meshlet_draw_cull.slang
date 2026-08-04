// Brief 04c — DRAW PHASE of the two-phase GPU cull for the meshlet worklist path. One thread per
// draw over the DrawInfo table. Per resident+visible draw it selects a LOD (screen-space-error,
// ported verbatim from the old CPU update() math) and writes, at the STABLE slot == draw index:
//   - counts[d]          : that draw's surviving-for-this-list meshlet count (0 = culled/skipped)
//   - counts_twosided[d] : same, for the two-sided partition (the other partition gets 0)
//   - draw_lod[d]        : the draw's selected LOD (shared by every camera-derived list; shadows too)
// The EXPANSION shader (meshlet_expand.slang) then prefix-scans counts[] into contiguous worklist
// entries + the indirect groupCount. The per-meshlet frustum/cone/HiZ cull still runs in the task
// shader, so this compute keeps its draw-level frustum test only (meshlet-level stats stay comparable
// to the pre-04c path). ORDER-STABLE: the scan preserves draw order, so submission order is
// deterministic (the 03b z-fighting constraint).
import meshlet;
import meshlet_cull;   // brief 04d M1: shared frustum + LOD-select primitives (was inlined here)

struct Push
{
    float4x4 cull_view_proj;            // draw-level frustum source (frozen-aware; C disables)
    DrawInfoBuffer *draws;              // per-draw table (resident flag = streaming gate)
    UintBuffer *counts;                 // out: per-draw meshlet count, ONE-SIDED list (stable slot d)
    UintBuffer *counts_twosided;        // out: per-draw meshlet count, TWO-SIDED list (stable slot d)
    UintBuffer *counts_shadow;          // out: per-draw meshlet count, resident-only shadow list
    UintBuffer *draw_lod;               // out: per-draw selected LOD (camera-derived lists share it)
    uint *stats;                        // GpuMeshStats: LOD histogram lands here (stats[4+lod])
    uint draw_count;                    // active_draw_count_
    uint lod_enabled;                   // 0 -> force LOD0
    uint frustum_cull;                  // 0 = C toggle off -> pass everything
    uint stats_lod;                     // 1 -> write the LOD histogram (camera pass only)
    float3 camera_pos;                  // frozen-aware eye (screen-space-error LOD metric)
    float lod_error_px;                 // kLodErrorPx (0.02, or STRING_LOD_PX)
    float focal;                        // half_h / tan(fov/2), matches the CPU code
    uint shadow_mode;                   // brief 04 M4: 1 -> draw-cull shadow list vs the cascade sphere
    float shadow_radius;                // brief 04 M4: cascade bounding-sphere radius (world units)
    uint force_lod0;                    // 1 -> prepass: force LOD0 counts (fullest silhouette for HiZ)
    int isolate_draw;                   // brief 06: >=0 -> keep ONLY this draw index; -1 = off
    float3 shadow_center;               // brief 04 M4: cascade bounding-sphere centre (world space)
    float _pad4;
};
[[vk::push_constant]] Push pc;

// Camera perspective draw-cull: shared Gribb-Hartmann 6-plane sphere test (meshlet_cull module).
bool draw_in_frustum(float3 c, float r)
{
    float4 planes[6];
    frustum_planes(pc.cull_view_proj, planes);
    return sphere_in_frustum(planes, c, r);
}

// Draw-level sphere-vs-CASCADE test (brief 04 M4): conservative world-space sphere-sphere overlap.
bool sphere_in_cascade(float3 cc, float cr, float3 c, float r)
{
    float3 d = c - cc;
    float sum = r + cr;
    return dot(d, d) <= sum * sum;
}

[shader("compute")]
[numthreads(64)]
void cull_main(uint3 id : SV_DispatchThreadID)
{
    uint d = id.x;
    if (d >= pc.draw_count) return;

    DrawInfo di = pc.draws.draws[d];

    uint lod = 0;
    uint mcount = 0;   // this draw's meshlet count at the selected LOD (0 if not resident/empty)

    if (di.resident != 0 && di.lod_count != 0)
    {
        // --- LOD select (shared meshlet_cull::select_lod — screen-space-error metric) ---
        // di.center is WORLD-space (the loader transformed the AABB) — no di.model applied.
        if (pc.force_lod0 == 0 && pc.lod_enabled != 0 && di.lod_count > 1)
            lod = select_lod(di.lods, di.lod_count, di.center, pc.camera_pos, pc.focal, pc.lod_error_px);
        lod = min(lod, di.lod_count - 1);
        mcount = di.lods[lod].meshlet_count;
        if (mcount != 0 && pc.stats_lod != 0 && lod < 4)
            InterlockedAdd(pc.stats[4 + lod], 1u);   // LOD histogram (camera pass only)
    }

    pc.draw_lod.v[d] = lod;

    // Brief 06 isolate-draw: when set, every draw except the chosen index contributes 0 meshlets to
    // every list (camera one-sided/two-sided AND shadow), so only that draw renders in-world.
    if (pc.isolate_draw >= 0 && int(d) != pc.isolate_draw)
        mcount = 0u;

    // Brief 04: alpha-blend draws are excluded from ALL opaque lists here — they render in the
    // separate sorted transparency pass (CPU-built list). They also don't cast a solid shadow.
    bool blend = (di.flags & kDrawFlagBlend) != 0u;

    // Shadow list. Off-camera casters must survive -> NEVER cull against the camera frustum. When
    // shadow_mode is set, reject draws whose world sphere is outside THIS cascade's world sphere.
    uint shadow_count = blend ? 0u : mcount;
    if (pc.shadow_mode != 0u && shadow_count != 0u)
    {
        if (!sphere_in_cascade(pc.shadow_center, pc.shadow_radius, di.center, di.radius))
            shadow_count = 0u;
        else
            InterlockedAdd(pc.stats[9], 1u);   // surviving shadow draw (summed across cascades)
    }
    pc.counts_shadow.v[d] = shadow_count;

    // Camera list: draw-level frustum cull on the draw's world-space bounding sphere.
    uint cam_count = mcount;
    if (cam_count != 0u && pc.frustum_cull != 0u && !draw_in_frustum(di.center, di.radius))
        cam_count = 0u;

    // Partition the camera list by facing into a one-sided and a two-sided count (the other is 0).
    bool two_sided = (di.flags & kDrawFlagDoubleSided) != 0u;
    pc.counts.v[d]          = (blend || two_sided) ? 0u : cam_count;
    pc.counts_twosided.v[d] = (!blend && two_sided) ? cam_count : 0u;
}
