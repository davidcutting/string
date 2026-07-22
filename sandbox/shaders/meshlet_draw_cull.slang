// Brief 03b — GPU draw-cull compute for the meshlet path. One thread per
// draw over the DrawInfo table. Per resident draw it selects a LOD (screen-space-error, ported
// verbatim from the old CPU update() math) and atomically appends a VkDrawMeshTasksIndirectCommandEXT
// + a {draw_index, lod} record to a work list the CPU then issues with one
// vkCmdDrawMeshTasksIndirectCountEXT. The per-meshlet frustum/cone/HiZ cull stays in the task shader,
// so this compute does NO draw-level frustum cull: that keeps the meshlet-level stats (meshlets_total
// etc.) bit-identical to the old per-draw dispatch loop, which also dispatched every resident draw.
import meshlet;

// Modes: 0 = camera list (does LOD select). 1 = shadow cascade list (reuses camera-selected LOD via
// the records the caller doesn't overwrite — see note; here shadow reuses this same shader with a
// per-cascade light_view_proj but keeps LOD from the CPU-independent formula at mode 0). To keep the
// shadow LOD choice identical to the old code (which used the camera-selected draw_lod_ for cascades)
// the shadow dispatch runs mode 0 too but with cull_view_proj = camera (LOD select is camera-based)
// and appends every resident draw — the per-cascade meshlet frustum cull happens in the shadow task
// shader. mode is kept for a future light-frustum draw-level reject (not needed for parity).
struct Push
{
    float4x4 cull_view_proj;            // draw-level frustum source (frozen-aware; C disables)
    DrawInfoBuffer *draws;              // per-draw table (resident flag = streaming gate)
    MeshTaskCommandBuffer *commands;    // out: frustum-culled list (main pass / HiZ prepass)
    MeshTaskCommandBuffer *shadow_commands; // out: resident-only list (cascades — casters may be off-camera)
    DrawRecordBuffer *records;          // out: parallel {draw_index, lod}
    CountBuffer *count;                 // out: number appended (indirect count)
    uint *stats;                        // GpuMeshStats: LOD histogram lands here (stats[4+lod])
    uint draw_count;                    // active_draw_count_
    uint lod_enabled;                   // 0 -> force LOD0
    uint frustum_cull;                  // 0 = C toggle off -> pass everything
    uint stats_lod;                     // 1 -> write the LOD histogram (camera pass only)
    float3 camera_pos;                  // frozen-aware eye (screen-space-error LOD metric)
    float lod_error_px;                 // kLodErrorPx (0.02, or STRING_LOD_PX)
    float focal;                        // half_h / tan(fov/2), matches the CPU code
    uint _pad0;
    uint _pad1;
    uint _pad2;
};
[[vk::push_constant]] Push pc;

// Draw-level sphere-vs-frustum (Gribb-Hartmann; Slang m[i] is a ROW — see meshlet_mesh.slang).
bool draw_in_frustum(float3 c, float r)
{
    float4 r0 = pc.cull_view_proj[0];
    float4 r1 = pc.cull_view_proj[1];
    float4 r2 = pc.cull_view_proj[2];
    float4 r3 = pc.cull_view_proj[3];
    float4 planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float3 n = planes[i].xyz;
        float len = length(n);
        if (len < 1e-6) continue;
        if ((dot(n, c) + planes[i].w) / len < -r) return false;
    }
    return true;
}

[shader("compute")]
[numthreads(64)]
void cull_main(uint3 id : SV_DispatchThreadID)
{
    uint d = id.x;
    if (d >= pc.draw_count) return;

    // The last thread publishes the count = draw_count. The command list is written at a STABLE slot
    // (== draw index), NOT atomically compacted: a compacted list's order depends on workgroup race
    // order, which changed the submission order of coplanar meshlets frame-to-frame and produced
    // non-deterministic z-fighting. Emitting one command per draw (groupCountX=0 for skipped draws)
    // keeps the draw order identical to the old sequential per-draw dispatch loop (deterministic,
    // pixel-stable, matches the old vertex path's order). Skipped draws cost an empty task dispatch.
    if (d == pc.draw_count - 1) pc.count.count = pc.draw_count;

    DrawInfo di = pc.draws.draws[d];

    MeshTaskCommand cmd; cmd.x = 0u; cmd.y = 1u; cmd.z = 1u;
    DrawRecord rec; rec.draw_index = d; rec.lod = 0u;

    if (di.resident != 0 && di.lod_count != 0)
    {
        // --- LOD select (verbatim port of the CPU screen-space-error math in geometry_pass.cpp) ---
        uint lod = 0;
        if (pc.lod_enabled != 0 && di.lod_count > 1)
        {
            // DrawInfo.center/radius are WORLD-space (the loader transforms the AABB corners at
            // flatten; meshlet_builder copies that AABB) — do NOT re-apply di.model here.
            float3 center_ws = di.center;
            float dist = max(length(center_ws - pc.camera_pos), 0.01);
            float px_per_unit = pc.focal / dist;
            uint pick = 0;
            for (uint l = 0; l < di.lod_count; ++l)
                if (di.lods[l].error * px_per_unit <= pc.lod_error_px) pick = l;
            lod = pick;
        }
        lod = min(lod, di.lod_count - 1);
        uint mcount = di.lods[lod].meshlet_count;
        if (mcount != 0)
        {
            cmd.x = (mcount + 31u) / 32u;   // ceil(meshlet_count / kTaskGroup)
            rec.lod = lod;
            // LOD histogram (camera pass only — matches the old task shader's stats[4+lod] per draw).
            if (pc.stats_lod != 0 && lod < 4)
                InterlockedAdd(pc.stats[4 + lod], 1u);
        }
    }

    // Shadow list: resident-only (no camera frustum — off-screen casters still cast into view).
    pc.shadow_commands.cmds[d] = cmd;

    // Camera list: draw-level frustum cull on the draw's world-space bounding sphere. This skips
    // whole task workgroup ranges before they launch — the cheapest cull in the pipeline. The
    // per-meshlet frustum test in the task shader stays (it culls within partially-visible draws).
    if (cmd.x != 0u && pc.frustum_cull != 0u)
    {
        // center/radius are already WORLD-space (see LOD note above) — no model transform.
        if (!draw_in_frustum(di.center, di.radius))
            cmd.x = 0u;
    }
    pc.commands.cmds[d] = cmd;
    pc.records.records[d] = rec;
}
