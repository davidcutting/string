// Depth-only meshlet shadow path (brief 03): task + mesh, one cascade per dispatch. Frustum + cone
// cull per cascade (no HiZ for light views in v1). Replaces the interim indirect shadow draw list.
// Alpha-cutout is out of scope until brief 04 (no cutout materials yet).
import meshlet;

static const uint kTaskGroup = 32;

struct Push
{
    float4x4 light_view_proj;    // this cascade's ortho view-projection
    VertexBuffer *vertices;
    MeshletBuffer *meshlets;
    MeshletVertexBuffer *mverts;
    MeshletTriangleBuffer *mtris;
    DrawInfoBuffer *draws;
    DrawRecordBuffer *records;   // brief 03b: {draw_index, lod} per indirect draw (SV_DrawIndex)
    uint _pad0;
    uint _pad1;
};
[[vk::push_constant]] Push pc;

struct Payload { uint meshlet_ids[kTaskGroup]; uint draw_index; };
groupshared Payload payload;

void frustum_planes(float4x4 vp, out float4 planes[6])
{
    // Slang/HLSL: vp[i] IS row i (unlike GLSL, where m[i] is a column). See meshlet_mesh.slang.
    float4 r0 = vp[0];
    float4 r1 = vp[1];
    float4 r2 = vp[2];
    float4 r3 = vp[3];
    planes[0] = r3 + r0; planes[1] = r3 - r0;
    planes[2] = r3 + r1; planes[3] = r3 - r1;
    planes[4] = r2;      planes[5] = r3 - r2;
}
bool sphere_in_frustum(float4 planes[6], float3 c, float r)
{
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float3 n = planes[i].xyz; float len = length(n);
        if (len < 1e-6) continue;
        if ((dot(n, c) + planes[i].w) / len < -r) return false;
    }
    return true;
}

[shader("amplification")]
[numthreads(kTaskGroup)]
void task_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID, uint draw : SV_DrawIndex)
{
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

    uint slot = WavePrefixCountBits(survive);
    uint total = WaveActiveCountBits(survive);
    if (survive) payload.meshlet_ids[slot] = global_id;
    DispatchMesh(total, 1, 1, payload);
}

struct MeshOut { float4 position : SV_Position; };

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
        verts[tid] = o;
    }
    for (uint t = tid; t < ml.triangle_count; t += kMeshletMaxVertices)
    {
        uint packed = pc.mtris.packed[ml.triangle_offset + t];
        tris[t] = uint3(packed & 0xffu, (packed >> 8) & 0xffu, (packed >> 16) & 0xffu);
    }
}
