// Task + mesh + fragment meshlet draw path (brief 03 / 04c resolution b). ONE indirect draw command
// per surviving draw (via vkCmdDrawMeshTasksIndirectCountEXT over the compacted commands[]/records[]
// the expand compute built); groupCountX = ceil(selected-LOD meshlet count / kTaskGroup). The task
// shader reads its draw via SV_DrawIndex -> records[], tests each meshlet (frustum sphere, backface
// cone, optional HiZ) and emits survivors to the mesh shader via payload; the mesh shader pulls the
// meshlet's vertices from the shared heap, transforms, and emits <=64 verts / <=124 tris. The
// fragment reuses lighting.slang::shade_surface — ONE lighting path.
//
// Per-draw command boundaries (one command per surviving draw) are what pin coplanar depth-tie
// winners run-to-run on RADV/RDNA3: multi-command IndirectCountEXT is ordered by command index per
// spec (measured AE=0), whereas a single mesh-shader dispatch across all meshlets is NOT ordered
// between mesh workgroups (the ~0.1% floor shimmer). See meshlet_expand.slang for the compaction.
import lighting;
import meshlet;
import meshlet_cull;   // brief 04d M1: shared frustum/cone/HiZ primitives (was inlined here)

static const uint kTaskGroup = 32;   // meshlets tested per task workgroup

// Debug-view flags (Push.debug_view): 0 none, 1 meshlet-id colour, 2 LOD colour, 3 occlusion (HiZ
// failures drawn — handled by a separate reject pass), plus freeze-cull handled via Push.cull_*.
struct Push
{
    float4x4 view_proj;          // camera view-projection (mesh vertex transform + screen)
    float4x4 cull_view_proj;     // frustum source (== view_proj, or frozen for freeze-cull debug)
    VertexBuffer *vertices;      // shared vertex heap (global indices)
    MeshletBuffer *meshlets;     // global meshlet buffer
    MeshletVertexBuffer *mverts; // meshlet-vertex remap heap (global vertex indices)
    MeshletTriangleBuffer *mtris;// packed local triangle heap
    DrawInfoBuffer *draws;       // per-draw table
    SceneData *scene;            // lighting/shadow/froxel state
    uint *stats;                 // GpuMeshStats (atomically incremented)
    DrawRecordBuffer *records;   // brief 04c: compacted {draw_index, lod} per indirect draw (SV_DrawIndex)
    float3 camera_pos;   float _pad_cp;
    uint debug_view;             // 0 none / 1 meshlet / 2 lod / 3 occlusion-reject
    uint hiz_slot;               // bindless slot of the HiZ pyramid (pass 2)
    uint hiz_mips;               // pyramid mip count
    uint _pad_hiz;
    uint2 hiz_size;              // pyramid mip-0 dimensions (px)
    uint blend_pass;             // brief 04: 1 = transparency pass -> fragment returns base-color alpha
    // Brief 04d two-phase occlusion. phase: 0 = single-pass (HiZ-off / transparency: no bitfield), 1 =
    // render meshlets whose bit is SET (visible last frame; NO HiZ test), 2 = render the complement
    // (bit CLEAR) that is HiZ-visible, and update the bit. bitfield = persistent per-meshlet bits
    // (1 bit / global meshlet id). freeze_bits: 1 = do not mutate bits (F freeze).
    uint phase;
    VisBitfield *bitfield;
    uint freeze_bits;
    uint _pad_end2;
};
[[vk::push_constant]] Push pc;

// Payload from task -> mesh: the surviving meshlet ids (global) for this workgroup, plus this
// draw's record (draw_index + selected LOD) resolved from SV_DrawIndex in the task shader.
struct Payload
{
    uint meshlet_ids[kTaskGroup];
    uint draw_index;
    uint lod;
};
groupshared Payload payload;

// LOD-relative meshlet id -> global meshlet id, for the draw's selected LOD.
uint global_meshlet_id(DrawInfo di, uint lod, uint local)
{
    return di.lods[lod].meshlet_offset + local;
}

[shader("amplification")]
[numthreads(kTaskGroup)]
void task_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID, uint draw : SV_DrawIndex)
{
    // Brief 04c: the expand compute wrote a compacted {draw_index, lod} record per surviving draw;
    // SV_DrawIndex is the index into records[] (shaderDrawParameters enabled). LOD histogram lives in
    // the cull compute (once per draw), so it is NOT written here.
    DrawRecord rec = pc.records.records[draw];
    uint draw_index = rec.draw_index;
    uint lod = rec.lod;
    DrawInfo di = pc.draws.draws[draw_index];
    lod = min(lod, di.lod_count > 0 ? di.lod_count - 1 : 0);
    uint lod_count = di.lods[lod].meshlet_count;

    payload.draw_index = draw_index;
    payload.lod = lod;

    uint local = gid.x * kTaskGroup + tid;
    bool survive = false;
    uint global_id = 0;

    if (di.resident != 0 && local < lod_count)
    {
        global_id = global_meshlet_id(di, lod, local);
        Meshlet ml = pc.meshlets.meshlets[global_id];

        // Transform bounds to world space (model transform; radius scaled by max axis length).
        float3 center_ws = mul(di.model, float4(ml.center, 1.0)).xyz;
        float3 sx = float3(di.model[0][0], di.model[1][0], di.model[2][0]);
        float3 sy = float3(di.model[0][1], di.model[1][1], di.model[2][1]);
        float3 sz = float3(di.model[0][2], di.model[1][2], di.model[2][2]);
        float smax = sqrt(max(max(dot(sx,sx), dot(sy,sy)), dot(sz,sz)));
        float radius_ws = ml.radius * smax;

        // Count this meshlet as considered.
        InterlockedAdd(pc.stats[0], 1u);

        float4 planes[6];
        frustum_planes(pc.cull_view_proj, planes);
        bool in_frustum = sphere_in_frustum(planes, center_ws, radius_ws);
        if (in_frustum) InterlockedAdd(pc.stats[1], 1u);

        // Backface cone cull (skinned + two-sided draws bypass — bounds/cone are static).
        // meshlet_cull::cone_backfacing is the shared meshopt CENTER-based reference test.
        bool cone_ok = true;
        if (in_frustum && di.skinned == 0 && (di.flags & kDrawFlagDoubleSided) == 0u && ml.cone_cutoff < 1.0)
        {
            float3 axis_ws = normalize(mul((float3x3)di.model, ml.cone_axis));
            if (cone_backfacing(axis_ws, ml.cone_cutoff, center_ws, radius_ws, pc.camera_pos))
                cone_ok = false;
        }
        if (in_frustum && cone_ok) InterlockedAdd(pc.stats[2], 1u);

        bool visible = in_frustum && cone_ok;   // passed the per-meshlet frustum + cone gate
        if (visible)
        {
            if (pc.phase == 1u)
            {
                // Phase 1: render ONLY meshlets marked visible LAST frame (bit set). No HiZ test — the
                // set is by definition depth-correct occluders; testing them would need a pyramid we
                // haven't built yet. Bits are NOT mutated here (phase 2 owns the update).
                survive = vis_get(pc.bitfield, global_id);
                if (survive) InterlockedAdd(pc.stats[3], 1u);   // counted as drawn (phase1)
            }
            else if (pc.phase == 2u)
            {
                // Phase 2: test EVERY frustum+cone survivor against the fresh pyramid to UPDATE its bit
                // (so a now-occluded meshlet clears + stops rendering in phase 1 next frame), but only
                // DRAW the newly-visible COMPLEMENT (bit was CLEAR). Bit-SET meshlets were already drawn
                // in phase 1 — re-drawing them would double-raster and break determinism, so phase 2
                // never draws them; it only re-tests to keep their bit current. This keeps the drawn set
                // == the truly-visible set while phase-2 submission stays the disocclusion complement.
                bool was_set = vis_get(pc.bitfield, global_id);
                bool hv = hiz_visible(lit_textures, pc.hiz_slot, pc.hiz_size, pc.hiz_mips,
                                      pc.cull_view_proj, pc.camera_pos, center_ws, radius_ws);
                if (was_set)
                {
                    // Already drawn in phase 1; just keep the bit honest for next frame.
                    if (pc.freeze_bits == 0u && !hv) vis_clear(pc.bitfield, global_id);
                }
                else
                {
                    survive = hv;   // newly-visible complement -> draw it this frame
                    if (hv)
                    {
                        InterlockedAdd(pc.stats[3], 1u);          // counted as drawn (phase2)
                        InterlockedAdd(pc.stats[10], 1u);         // phase-2 drawn (disocclusion set)
                        if (pc.freeze_bits == 0u) vis_set(pc.bitfield, global_id);
                        global_id |= 0x80000000u;   // phase-2 tint flag (bit 31; global ids never use it)
                    }
                    // (HiZ-hidden + already-clear: leave the bit clear; retest next frame.)
                }
                // Occlusion-reject debug view (3): show phase-2 HiZ-rejected complement meshlets red.
                if (pc.debug_view == 3 && !was_set && !survive)
                {
                    survive = true;
                    global_id = (global_id & 0x7fffffffu) | 0x80000000u;
                }
            }
            else
            {
                // Phase 0 (legacy single-pass: HiZ disabled OR transparency): frustum+cone+HiZ, no bits.
                bool hv = hiz_visible(lit_textures, pc.hiz_slot, pc.hiz_size, pc.hiz_mips,
                                      pc.cull_view_proj, pc.camera_pos, center_ws, radius_ws);
                survive = hv;
                if (hv) InterlockedAdd(pc.stats[3], 1u);
                if (pc.debug_view == 3 && !hv)
                {
                    survive = true;
                    global_id |= 0x80000000u;   // reject flag (bit 31)
                }
            }
        }
    }

    // Compact survivors into the payload with a wave/prefix count.
    uint slot = WavePrefixCountBits(survive);
    uint total = WaveActiveCountBits(survive);
    if (survive) payload.meshlet_ids[slot] = global_id;

    DispatchMesh(total, 1, 1, payload);
}

struct MeshOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float3 world_tangent : TEXCOORD3;
    float tangent_sign : TEXCOORD4;
    nointerpolation float4 base_color : TEXCOORD5;
    nointerpolation uint4 slots : TEXCOORD6;
    nointerpolation float2 metal_rough : TEXCOORD7;
    nointerpolation uint dbg : TEXCOORD8;   // meshlet id / lod, for the debug views
    nointerpolation uint rejected : TEXCOORD9;  // 1 = HiZ-rejected (occlusion-reject view tints red)
    nointerpolation float cutout_alpha : TEXCOORD10;  // brief 04: >=0 alpha-test threshold, <0 = off
};

void unpack_tangent_m(uint packed, out float3 tangent, out float sign)
{
    int xi = int(packed << 22) >> 22;
    int yi = int(packed << 12) >> 22;
    int zi = int(packed << 2) >> 22;
    int wi = int(packed << 0) >> 30;
    tangent = float3(max(xi / 511.0, -1.0), max(yi / 511.0, -1.0), max(zi / 511.0, -1.0));
    sign = wi < 0 ? -1.0 : 1.0;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kMeshletMaxVertices)]
void mesh_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID,
               in payload Payload pl,
               out vertices MeshOut verts[kMeshletMaxVertices],
               out indices uint3 tris[kMeshletMaxTriangles])
{
    uint raw_id = pl.meshlet_ids[gid.x];
    uint rejected = (raw_id >> 31) & 1u;           // occlusion-reject view flag (bit 31)
    uint global_id = raw_id & 0x7fffffffu;
    Meshlet ml = pc.meshlets.meshlets[global_id];
    DrawInfo di = pc.draws.draws[pl.draw_index];

    SetMeshOutputCounts(ml.vertex_count, ml.triangle_count);

    float3x3 nmat = (float3x3)di.model;

    // Emit vertices (one lane per vertex; up to 64).
    if (tid < ml.vertex_count)
    {
        // Rebase the ORIGINAL global vertex index into the suballocated heap (see DrawInfo).
        uint gv = uint(int(pc.mverts.indices[ml.vertex_offset + tid]) + int(di.vertex_offset));
        Vertex v = pc.vertices.verts[gv];
        float4 world = mul(di.model, float4(v.pos, 1.0));

        float3 t; float sign;
        unpack_tangent_m(v.tangent, t, sign);

        MeshOut o;
        o.position = mul(pc.view_proj, world);
        o.uv = v.texCoord;
        o.world_pos = world.xyz;
        o.world_normal = mul(nmat, v.normal);
        o.world_tangent = mul(nmat, t);
        o.tangent_sign = sign;
        o.base_color = di.base_color;
        o.slots = uint4(di.base_slot, di.normal_slot, di.mr_slot, di.occlusion_slot);
        o.metal_rough = float2(di.metallic, di.roughness);
        o.dbg = pc.debug_view == 2 ? pl.lod : global_id;
        o.rejected = rejected;
        o.cutout_alpha = (di.flags & kDrawFlagCutout) != 0u ? di.alpha_cutoff : -1.0;
        verts[tid] = o;
    }

    // Emit triangles (one lane per triangle for the first 64; loop for the tail up to 124).
    for (uint t = tid; t < ml.triangle_count; t += kMeshletMaxVertices)
    {
        uint packed = pc.mtris.packed[ml.triangle_offset + t];
        tris[t] = uint3(packed & 0xffu, (packed >> 8) & 0xffu, (packed >> 16) & 0xffu);
    }
}

// A small palette for the meshlet/LOD debug views (hash -> distinct hue).
float3 debug_color(uint id)
{
    uint h = id * 2654435761u;
    return float3(float((h >> 0) & 0xff), float((h >> 8) & 0xff), float((h >> 16) & 0xff)) / 255.0;
}

[shader("fragment")]
float4 fragment_main(MeshOut i) : SV_Target
{
    Surface surf;
    surf.uv = i.uv;
    surf.world_pos = i.world_pos;
    surf.world_normal = i.world_normal;
    surf.world_tangent = i.world_tangent;
    surf.tangent_sign = i.tangent_sign;
    surf.base_color = i.base_color;
    surf.slots = i.slots;
    surf.metal_rough = i.metal_rough;
    surf.frag_coord = i.position;
    surf.cutout_alpha = i.cutout_alpha;

    float4 tint = float4(0);
    if (pc.debug_view == 1) tint = float4(debug_color(i.dbg), 0.85);        // meshlet-id colour
    else if (pc.debug_view == 2) tint = float4(debug_color(i.dbg * 97u + 3u), 0.85);  // LOD colour
    else if (pc.debug_view == 3 && i.rejected != 0u) tint = float4(1.0, 0.0, 0.0, 0.85);  // occlusion-rejected -> red
    // Brief 04d view 4: phase-2-drawn meshlets (the disocclusion set) tinted cyan. Under motion this
    // set should be SMALL and hug occluder silhouettes / screen edges; a full-screen tint = broken bits.
    else if (pc.debug_view == 4 && i.rejected != 0u) tint = float4(0.0, 1.0, 1.0, 0.85);
    return shade_surface(*pc.scene, surf, tint, pc.blend_pass != 0u);
}
