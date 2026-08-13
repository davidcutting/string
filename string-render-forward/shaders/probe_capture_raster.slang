// Brief 09b — per-probe cube-face G-buffer raster (STATIC, load-time), GPU-driven + multiview.
//
// All 6 cube faces render in ONE render pass (VK_KHR_multiview, viewMask=0x3F): the pass broadcasts
// each mesh workgroup to all 6 views and the mesh shader reads `SV_ViewID` to pick that face's
// view-projection (pc.view_proj[6]). This removes the old per-face BeginRendering + CPU per-draw
// loop entirely — the whole probe is one vkCmdDrawMeshTasksEXT.
//
// The dispatch is a SINGLE flat grid over ALL LOD0 meshlets of the scene:
//   grid = (ceil(total_lod0_meshlets / 32), 1, 1)
// The task shader maps its flat (group,thread) id -> a GLOBAL meshlet id, resolves which draw it
// belongs to via a GPU meshlet->draw table (pc.mdraw, built once at load), frustum-culls the meshlet
// against EACH of the 6 face frusta and builds a 6-bit visibility mask; it amplifies ONE mesh
// workgroup per surviving meshlet (survivors = visible in at least one face). The mesh shader, via
// multiview, then runs that workgroup once per face and emits nothing for faces whose mask bit is
// clear. No CPU-side culling or per-draw work generation: the culling is fully GPU-driven.
//
// Outputs a mini G-buffer per face layer:
//   MRT0 = albedo.rgb (post-texture base color, NO lighting)
//   MRT1 = world normal.xyz + linear hit distance (w)   (depth attachment resolves nearest surface)
import meshlet;
import meshlet_cull;

[[vk::binding(1, 0)]] Sampler2D cap_textures[];   // bindless base-color (shared set/binding with lighting)

// Flat capture index -> {draw index, global meshlet id}. Filled once at load (create_probe_resources)
// from the meshlet model, so the task shader resolves the draw + meshlet without a CPU per-draw loop.
struct MeshletDrawEntry { uint draw; uint meshlet; };
struct MeshletDrawBuffer { MeshletDrawEntry e[]; };
// Per-probe relocation offsets (matches probe_common ProbeOffsetBuffer). The raster ALWAYS adds
// offset[probe_index]: in bake round 1 it reads the zero-filled initial value (a probe's own offset
// is only written by its collapse, after this raster), in round 2 it reads the round-1 relocation —
// so round 2 re-captures from the RELOCATED position, keeping the visibility/hit distances
// consistent with probe_world() everywhere (relight hit reconstruction, Chebyshev, bounce).
struct OffsetBuffer { float4 offset[]; };

static const uint kTaskGroup = 32;

struct Push
{
    VertexBuffer *vertices;
    MeshletBuffer *meshlets;
    MeshletVertexBuffer *mverts;
    MeshletTriangleBuffer *mtris;
    DrawInfoBuffer *draws;
    MeshletDrawBuffer *mdraw;    // flat index -> {draw index, global meshlet id}
    float3 probe_pos;            // probe GRID position (relocation offset added GPU-side, see below)
    uint meshlet_count;          // total LOD0 meshlets across all draws (flat dispatch domain)
    float cull_far;              // distance cull radius: meshlets beyond this from the probe are skipped
    uint probe_index;            // this probe's linear index (offset lookup)
    OffsetBuffer *offsets;       // per-probe relocation (zero in round 1, round-1 value in round 2)
};
[[vk::push_constant]] Push pc;

// Effective capture centre: grid position + relocation offset (0 in bake round 1).
float3 probe_center()
{
    return pc.probe_pos + pc.offsets.offset[pc.probe_index].xyz;
}

// The 6 cube-face bases (forward, right, up), matching the C++ probe_face_basis() / ibl.slang
// face_dir(). Building the per-face view-projection here (from just probe_pos) keeps the push tiny —
// 6 mat4 would blow the 256B push budget.
static const float3 kFaceF[6] = { float3(1,0,0), float3(-1,0,0), float3(0,1,0),
                                  float3(0,-1,0), float3(0,0,1), float3(0,0,-1) };
static const float3 kFaceR[6] = { float3(0,0,-1), float3(0,0,1), float3(1,0,0),
                                  float3(1,0,0), float3(1,0,0), float3(-1,0,0) };
static const float3 kFaceU[6] = { float3(0,-1,0), float3(0,-1,0), float3(0,0,1),
                                  float3(0,0,-1), float3(0,-1,0), float3(0,-1,0) };

// Matches C++ probe_face_view_proj: clip = (r.v, u.v, near, f.v), v = worldpos - probe. 90deg FOV,
// reverse-Z (ndc.z = near/(f.v)). Column-major (mul(M, worldpos)); near = 0.05.
float4x4 face_view_proj(uint face)
{
    float3 f = kFaceF[face], r = kFaceR[face], u = kFaceU[face];
    float near_p = 0.05;
    float3 p = probe_center();
    // We want mul(M, world) = (dot(r, v), dot(u, v), near, dot(f, v)) with v = world - probe, matching
    // the C++ probe_face_view_proj. In Slang `mul(M, x).i == dot(M[i], x)` (M[i] is row i), so set the
    // rows directly.
    float4x4 m;
    m[0] = float4(r, -dot(r, p));
    m[1] = float4(u, -dot(u, p));
    m[2] = float4(0.0, 0.0, 0.0, near_p);
    m[3] = float4(f, -dot(f, p));
    return m;
}

// The task payload carries, per surviving meshlet: its global meshlet id, its draw index, and the
// 6-bit face visibility mask (bit f set => visible in face f). The mesh shader tests the mask against
// its own SV_ViewID.
struct Payload
{
    uint meshlet_ids[kTaskGroup];
    uint draw_ids[kTaskGroup];
    uint face_mask[kTaskGroup];
};
groupshared Payload payload;
// The 6 face frusta depend ONLY on probe_pos (identical for every thread in the whole dispatch), so
// compute them ONCE per workgroup into groupshared instead of 6x per meshlet per thread (a ~32x cut
// in the dominant cull arithmetic — the "why is the bake heavy" fix). 6 faces x 6 planes.
groupshared float4 gs_planes[6][6];
// Survivor ballot for the payload compaction (one bit per thread, so kTaskGroup must stay <= 32).
// Workgroup-scoped on purpose — see the long note on gs_task_ballot in meshlet_mesh.slang for why
// WavePrefixCountBits/WaveActiveCountBits are the wrong scope for a workgroup-shared payload.
groupshared uint gs_task_ballot;

[shader("amplification")]
[numthreads(kTaskGroup)]
void task_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID)
{
    // Clear the survivor ballot; the barrier below (for gs_planes) publishes it to the workgroup.
    if (tid == 0) gs_task_ballot = 0u;

    // Cooperatively build the 6 face frusta: threads 0..5 each own one face.
    if (tid < 6)
    {
        float4 planes[6];
        frustum_planes(face_view_proj(tid), planes);
        [unroll] for (uint p = 0; p < 6; ++p) gs_planes[tid][p] = planes[p];
    }
    GroupMemoryBarrierWithGroupSync();

    uint flat = gid.x * kTaskGroup + tid;   // global LOD0 meshlet index (flat dispatch)

    bool survive = false;
    uint global_id = 0;
    uint draw_index = 0;
    uint mask = 0;
    if (flat < pc.meshlet_count)
    {
        MeshletDrawEntry e = pc.mdraw.e[flat];
        draw_index = e.draw;
        DrawInfo di = pc.draws.draws[draw_index];
        if (di.resident != 0)
        {
            global_id = e.meshlet;
            Meshlet ml = pc.meshlets.meshlets[global_id];
            float3 center_ws = mul(di.model, float4(ml.center, 1.0)).xyz;
            float3 sx = float3(di.model[0][0], di.model[1][0], di.model[2][0]);
            float3 sy = float3(di.model[0][1], di.model[1][1], di.model[2][1]);
            float3 sz = float3(di.model[0][2], di.model[1][2], di.model[2][2]);
            float smax = sqrt(max(max(dot(sx,sx), dot(sy,sy)), dot(sz,sz)));
            float rad = ml.radius * smax;
            // DISTANCE CULL FIRST (the dominant win): a probe's diffuse GI is dominated by nearby
            // geometry, so skip meshlets whose sphere is entirely beyond cull_far from the probe. Over
            // Sponza's 122k meshlets this rejects the vast majority per probe before any frustum math —
            // the capture cost was O(all_meshlets x all_probes) without it.
            if (distance(center_ws, probe_center()) - rad <= pc.cull_far)
            {
            // Frustum-cull against each of the 6 face 90-degree views (frusta precomputed above); only
            // ~1/6 of the scene falls in any one cone, so this keeps the raster work ~visibility-bound.
            for (uint f = 0; f < 6; ++f)
            {
                float4 planes[6];
                [unroll] for (uint p = 0; p < 6; ++p) planes[p] = gs_planes[f][p];
                if (sphere_in_frustum(planes, center_ws, rad)) mask |= (1u << f);
            }
            }
            survive = mask != 0u;
        }
    }

    if (survive) InterlockedOr(gs_task_ballot, 1u << tid);
    GroupMemoryBarrierWithGroupSync();
    const uint ballot = gs_task_ballot;
    const uint slot = countbits(ballot & ((1u << tid) - 1u));   // exclusive prefix == lane order
    const uint total = countbits(ballot);
    if (survive)
    {
        payload.meshlet_ids[slot] = global_id;
        payload.draw_ids[slot] = draw_index;
        payload.face_mask[slot] = mask;
    }
    DispatchMesh(total, 1, 1, payload);
}

struct MeshOut
{
    float4 position : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation float4 base_color : TEXCOORD3;
    nointerpolation uint base_slot : TEXCOORD4;
    nointerpolation float cutout_alpha : TEXCOORD5;   // >=0 alpha-test threshold, <0 = opaque
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kMeshletMaxVertices)]
void mesh_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID, uint view : SV_ViewID,
               in payload Payload pl,
               out vertices MeshOut verts[kMeshletMaxVertices],
               out indices uint3 tris[kMeshletMaxTriangles])
{
    // multiview: this workgroup runs once per face (SV_ViewID). Skip faces this meshlet doesn't touch.
    if ((pl.face_mask[gid.x] & (1u << view)) == 0u)
    {
        SetMeshOutputCounts(0, 0);
        return;
    }
    uint global_id = pl.meshlet_ids[gid.x];
    Meshlet ml = pc.meshlets.meshlets[global_id];
    DrawInfo di = pc.draws.draws[pl.draw_ids[gid.x]];
    SetMeshOutputCounts(ml.vertex_count, ml.triangle_count);

    float4x4 vp = face_view_proj(view);
    if (tid < ml.vertex_count)
    {
        // Fetch through THE vertex seam (meshlet.slang::load_vertex). The probe capture is a
        // static bake — skinned draws are excluded from its table — so the context stays null
        // by design, belt-and-braces with the CPU-side exclusion.
        SkinContext sc = { nullptr, nullptr };
        LoadedVertex v = load_vertex(sc, di, pc.mverts.indices[ml.vertex_offset + tid], pc.vertices);
        float4 world = mul(di.model, float4(v.pos, 1.0));
        MeshOut o;
        o.position = mul(vp, world);
        o.world_pos = world.xyz;
        o.world_normal = transform_normal(di.model, v.normal);   // inverse transpose — see meshlet.slang
        o.uv = v.uv;
        o.base_color = di.base_color;
        o.base_slot = di.base_slot;
        o.cutout_alpha = (di.flags & kDrawFlagCutout) != 0u ? di.alpha_cutoff : -1.0;
        verts[tid] = o;
    }
    for (uint t = tid; t < ml.triangle_count; t += kMeshletMaxVertices)
    {
        uint packed = pc.mtris.packed[ml.triangle_offset + t];
        tris[t] = uint3(packed & 0xffu, (packed >> 8) & 0xffu, (packed >> 16) & 0xffu);
    }
}

struct FragOut
{
    float4 albedo : SV_Target0;   // rgb post-texture base color
    float4 normal_dist : SV_Target1;   // xyz world normal (unnormalized ok), w linear hit distance
};

[shader("fragment")]
FragOut fragment_main(MeshOut i)
{
    float4 base = cap_textures[i.base_slot].Sample(i.uv) * i.base_color;
    if (i.cutout_alpha >= 0.0 && base.a < i.cutout_alpha) discard;
    FragOut o;
    o.albedo = float4(base.rgb, 1.0);
    o.normal_dist = float4(normalize(i.world_normal), length(i.world_pos - probe_center()));
    return o;
}
