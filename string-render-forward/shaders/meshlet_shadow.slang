// Depth-only meshlet shadow path (brief 03 / 04c resolution b): task + mesh, one cascade per dispatch.
// Frustum + cone cull per cascade (no HiZ for light views in v1). ONE indirect draw command per
// surviving draw (vkCmdDrawMeshTasksIndirectCountEXT over the compacted commands[]/records[] the
// expand compute built for this cascade); the task reads its draw via SV_DrawIndex -> records[].
// Brief 04: a fragment stage now applies alpha-tested cutout (clip on base-color alpha) so foliage/
// curtains cast and self-shadow correctly. This same module drives the HiZ depth prepass, so the
// prepass writes cutout depth too (HiZ would over-cull behind a cutout otherwise). Opaque draws
// carry a negative threshold and never discard, so their depth is bit-identical to the pre-04 path.
import meshlet;
import meshlet_cull;   // brief 04d M1: shared frustum primitives (was inlined here)

// Bindless base-color textures (same set/binding as lighting.slang; the pipeline layout is shared).
[[vk::binding(1, 0)]] Sampler2D shadow_textures[];

static const uint kTaskGroup = 32;

struct Push
{
    float4x4 light_view_proj;    // this cascade's ortho view-projection
    VertexBuffer *vertices;
    MeshletBuffer *meshlets;
    MeshletVertexBuffer *mverts;
    MeshletTriangleBuffer *mtris;
    DrawInfoBuffer *draws;
    DrawRecordBuffer *records;   // brief 04c: compacted {draw_index, lod} per indirect draw (SV_DrawIndex)
    uint _pad0;
    uint _pad1;
};
[[vk::push_constant]] Push pc;

struct Payload { uint meshlet_ids[kTaskGroup]; uint draw_index; };
groupshared Payload payload;
// Survivor ballot for the payload compaction (one bit per thread, so kTaskGroup must stay <= 32).
// Workgroup-scoped on purpose — see the long note on gs_task_ballot in meshlet_mesh.slang for why
// WavePrefixCountBits/WaveActiveCountBits are the wrong scope for a workgroup-shared payload.
groupshared uint gs_task_ballot;

[shader("amplification")]
[numthreads(kTaskGroup)]
void task_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID, uint draw : SV_DrawIndex)
{
    // Clear the survivor ballot before anyone votes (task_main has no early return, so uniform).
    if (tid == 0) gs_task_ballot = 0u;
    GroupMemoryBarrierWithGroupSync();

    DrawRecord rec = pc.records.records[draw];
    uint draw_index = rec.draw_index;
    DrawInfo di = pc.draws.draws[draw_index];
    uint lod = min(rec.lod, di.lod_count > 0 ? di.lod_count - 1 : 0);
    uint lod_count = di.lods[lod].meshlet_count;
    payload.draw_index = draw_index;
    uint local = gid.x * kTaskGroup + tid;

    bool survive = false;
    uint global_id = 0;
    if (di.resident != 0 && local < lod_count)
    {
        global_id = di.lods[lod].meshlet_offset + local;
        Meshlet ml = pc.meshlets.meshlets[global_id];
        float3 center_ws = mul(di.model, float4(ml.center, 1.0)).xyz;
        float3 sx = float3(di.model[0][0], di.model[1][0], di.model[2][0]);
        float3 sy = float3(di.model[0][1], di.model[1][1], di.model[2][1]);
        float3 sz = float3(di.model[0][2], di.model[1][2], di.model[2][2]);
        float smax = sqrt(max(max(dot(sx,sx), dot(sy,sy)), dot(sz,sz)));
        float4 planes[6];
        frustum_planes(pc.light_view_proj, planes);
        // No cone cull for shadows: a backfacing meshlet can still cast into the light view.
        survive = sphere_in_frustum(planes, center_ws, ml.radius * smax);
    }

    if (survive) InterlockedOr(gs_task_ballot, 1u << tid);
    GroupMemoryBarrierWithGroupSync();
    const uint ballot = gs_task_ballot;
    const uint slot = countbits(ballot & ((1u << tid) - 1u));   // exclusive prefix == lane order
    const uint total = countbits(ballot);
    if (survive) payload.meshlet_ids[slot] = global_id;
    DispatchMesh(total, 1, 1, payload);
}

struct MeshOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint base_slot : TEXCOORD1;
    nointerpolation float base_alpha : TEXCOORD2;    // base_color factor alpha
    nointerpolation float cutout_alpha : TEXCOORD3;  // >=0 alpha-test threshold, <0 = opaque (no clip)
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kMeshletMaxVertices)]
void mesh_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID,
               in payload Payload pl,
               out vertices MeshOut verts[kMeshletMaxVertices],
               out indices uint3 tris[kMeshletMaxTriangles])
{
    uint global_id = pl.meshlet_ids[gid.x];
    Meshlet ml = pc.meshlets.meshlets[global_id];
    DrawInfo di = pc.draws.draws[pl.draw_index];
    SetMeshOutputCounts(ml.vertex_count, ml.triangle_count);

    if (tid < ml.vertex_count)
    {
        // Rebase the ORIGINAL global vertex index into the suballocated heap (see DrawInfo).
        uint gv = uint(int(pc.mverts.indices[ml.vertex_offset + tid]) + int(di.vertex_offset));
        Vertex v = pc.vertices.verts[gv];
        MeshOut o;
        o.position = mul(pc.light_view_proj, mul(di.model, float4(v.pos, 1.0)));
        o.uv = v.texCoord;
        o.base_slot = di.base_slot;
        o.base_alpha = di.base_color.w;
        o.cutout_alpha = (di.flags & kDrawFlagCutout) != 0u ? di.alpha_cutoff : -1.0;
        verts[tid] = o;
    }
    for (uint t = tid; t < ml.triangle_count; t += kMeshletMaxVertices)
    {
        uint packed = pc.mtris.packed[ml.triangle_offset + t];
        tris[t] = uint3(packed & 0xffu, (packed >> 8) & 0xffu, (packed >> 16) & 0xffu);
    }
}

// Depth-only fragment: opaque draws (cutout_alpha < 0) fall straight through and write depth exactly
// as before; cutout draws sample base-color alpha and discard below the threshold (foliage/curtains).
[shader("fragment")]
void fragment_main(MeshOut i)
{
    if (i.cutout_alpha >= 0.0)
    {
        float a = shadow_textures[i.base_slot].Sample(i.uv).a * i.base_alpha;
        if (a < i.cutout_alpha) discard;
    }
}
