// Task + mesh + fragment meshlet draw path (brief 03). One vkCmdDrawMeshTasksEXT per resident draw;
// groupCountX = ceil(selected-LOD meshlet count / kTaskGroup). The task shader tests each meshlet
// (frustum sphere, backface cone, optional HiZ) and emits survivors to the mesh shader via payload;
// the mesh shader pulls the meshlet's vertices from the shared heap, transforms, and emits <=64
// verts / <=124 tris. The fragment reuses lighting.slang::shade_surface — ONE lighting path.
import lighting;
import meshlet;

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
    DrawRecordBuffer *records;   // brief 03b: {draw_index, lod} per indirect draw (SV_DrawIndex)
    float3 camera_pos;   float _pad_cp;
    uint debug_view;             // 0 none / 1 meshlet / 2 lod / 3 occlusion-reject
    uint hiz_slot;               // bindless slot of the HiZ pyramid (pass 2)
    uint hiz_mips;               // pyramid mip count
    uint2 hiz_size;              // pyramid mip-0 dimensions (px)
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

// Extract the 6 frustum planes from a view-projection (Gribb-Hartmann, ZERO_TO_ONE clip).
void frustum_planes(float4x4 vp, out float4 planes[6])
{
    // Slang/HLSL: vp[i] IS row i (unlike GLSL, where m[i] is a column — do NOT port the GLSL
    // transposed gather; MatrixLayoutColumn affects memory only, not subscript semantics).
    float4 r0 = vp[0];
    float4 r1 = vp[1];
    float4 r2 = vp[2];
    float4 r3 = vp[3];
    planes[0] = r3 + r0;  // left
    planes[1] = r3 - r0;  // right
    planes[2] = r3 + r1;  // bottom
    planes[3] = r3 - r1;  // top
    planes[4] = r2;       // near (reverse-Z: z>=0)
    planes[5] = r3 - r2;  // far
}

bool sphere_in_frustum(float4 planes[6], float3 c, float r)
{
    [unroll] for (int i = 0; i < 6; ++i)
    {
        float3 n = planes[i].xyz;
        float len = length(n);
        if (len < 1e-6) continue;
        if ((dot(n, c) + planes[i].w) / len < -r) return false;
    }
    return true;
}

// Project a world-space bounding sphere to a screen-space rect + CONSERVATIVE nearest depth, then
// test against the HiZ pyramid (reverse-Z: min-reduced = farthest). Returns true if potentially
// visible. Conservatism is mandatory: a false "hidden" here is a visible hole in the scene.
bool hiz_visible(float3 center_ws, float radius)
{
    if (pc.hiz_mips == 0) return true;   // HiZ off / not yet built -> everything passes

    // Inflate the test sphere a little: the min-reduced half-res pyramid + conservative-rect sampling
    // still leaves ~1px of silhouette-edge over-cull (a meshlet's visible sliver at a near occluder's
    // edge). Padding the bounds keeps those edge meshlets (occlusion must be invisible). Cost: a few
    // extra draws right at silhouettes.
    float r = radius * 1.2;

    // Nearest point of the sphere toward the camera — the sphere's closest (largest, reverse-Z)
    // depth. Using the CENTER depth here culls any meshlet whose front half pokes out from behind
    // an occluder (arches/walls) — that was a visible-holes bug.
    float3 to_c = center_ws - pc.camera_pos;
    float dist = length(to_c);
    if (dist <= r * 1.05) return true;   // camera inside (or grazing) the sphere — keep
    float4 clip_near = mul(pc.cull_view_proj, float4(center_ws - to_c * (r / dist), 1.0));
    if (clip_near.w <= 1e-4) return true;     // reaches the camera plane — keep
    float nearest = clip_near.z / clip_near.w;

    float4 clip = mul(pc.cull_view_proj, float4(center_ws, 1.0));
    if (clip.w <= 0.0) return true;
    float2 uv = (clip.xy / clip.w) * 0.5 + 0.5;

    // Screen-space half-extents: radius scaled by the projection diagonals (|VP row .xyz| = P00/P11
    // since the view rotation is norm-preserving) over view depth (= clip.w). NDC halfwidth -> uv.
    float2 sr = r * float2(length(pc.cull_view_proj[0].xyz), length(pc.cull_view_proj[1].xyz))
              / max(clip.w, 1e-4) * 0.5;
    // Rect touches the screen border -> keep (off-screen depth is unknown; clamping the rect
    // tests edge texels that don't cover the sphere's visible sliver — an edge-holes bug).
    if (any(uv - sr < 0.0) || any(uv + sr > 1.0)) return true;
    float2 uv_lo = clamp(uv - sr, 0.0, 1.0);
    float2 uv_hi = clamp(uv + sr, 0.0, 1.0);

    // Pick a mip where the rect spans at MOST ~4x4 texels, then sample every texel of that footprint
    // and take the FARTHEST (min). Sampling only 4 corners of the rect at a fine mip over-culls: the
    // true farthest depth can sit in an interior texel the corners miss, so occ reads too NEAR and the
    // meshlet is wrongly rejected (the ~3% interior over-cull). A coarser mip whose min-reduce already
    // folds the footprint's farthest into few texels, fully covered here, is conservative.
    float rect_px = max((uv_hi.x - uv_lo.x) * float(pc.hiz_size.x),
                        (uv_hi.y - uv_lo.y) * float(pc.hiz_size.y));
    // rect spans rect_px/2^mip texels at `mip`; choose mip so that is <= 4 (grid below covers 5x5).
    uint mip = uint(clamp(ceil(log2(max(rect_px * 0.25, 1.0))), 0.0, float(pc.hiz_mips - 1)));
    float occ = 1.0;
    [unroll] for (int y = 0; y <= 4; ++y)
    [unroll] for (int x = 0; x <= 4; ++x)
    {
        float2 uv_s = lerp(uv_lo, uv_hi, float2(x, y) * 0.25);
        occ = min(occ, lit_textures[pc.hiz_slot].SampleLevel(uv_s, float(mip)).r);
    }
    // reverse-Z: smaller depth = farther. Hidden only if our NEAREST point is behind the farthest
    // occluder across the whole rect. Small guard band absorbs half-res/precision slop at edges.
    return nearest >= occ - max(occ * 1e-3, 1e-5);
}

// LOD-relative meshlet id -> global meshlet id, for the draw's selected LOD.
uint global_meshlet_id(DrawInfo di, uint lod, uint local)
{
    return di.lods[lod].meshlet_offset + local;
}

[shader("amplification")]
[numthreads(kTaskGroup)]
void task_main(uint3 gid : SV_GroupID, uint tid : SV_GroupThreadID, uint draw : SV_DrawIndex)
{
    // Brief 03b: the draw-cull compute wrote {draw_index, lod} per indirect draw; SV_DrawIndex is
    // the index into records[] (shaderDrawParameters enabled). LOD histogram now lives in the cull
    // compute (once per draw), so it is NOT written here.
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

        // Backface cone cull (skinned draws bypass — bounds/cone are static).
        // meshopt's CENTER-based reference test: cull if
        //   dot(center - cam, axis) >= cutoff * length(center - cam) + radius
        // The +radius term keeps it conservative when using the sphere center instead of the
        // cone apex — without it, thin geometry at glancing angles is falsely culled.
        bool cone_ok = true;
        if (in_frustum && di.skinned == 0 && ml.cone_cutoff < 1.0)
        {
            float3 axis_ws = normalize(mul((float3x3)di.model, ml.cone_axis));
            float3 to_center = center_ws - pc.camera_pos;
            if (dot(to_center, axis_ws) >= ml.cone_cutoff * length(to_center) + radius_ws)
                cone_ok = false;
        }
        if (in_frustum && cone_ok) InterlockedAdd(pc.stats[2], 1u);

        // Single-pass HiZ: every frustum+cone-visible meshlet is tested against THIS frame's pyramid
        // (built from a complete depth prepass of all resident draws). No persistent visibility
        // bitfield / two-pass scheme — the prepass is complete, so one conservative test per meshlet
        // is correct and temporally stable (no dependence on last frame's draw set).
        bool visible = in_frustum && cone_ok;
        if (visible)
        {
            bool hv = hiz_visible(center_ws, radius_ws);   // true when HiZ off (hiz_mips==0)
            survive = hv;
            if (hv) InterlockedAdd(pc.stats[3], 1u);
            // Occlusion-reject debug view (3): force HiZ-rejected meshlets to survive too, flagged
            // via the payload's high bit so the fragment tints them red. Visible meshlets untinted.
            if (pc.debug_view == 3 && !hv)
            {
                survive = true;
                global_id |= 0x80000000u;   // reject flag (global ids never use bit 31)
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

    float4 tint = float4(0);
    if (pc.debug_view == 1) tint = float4(debug_color(i.dbg), 0.85);        // meshlet-id colour
    else if (pc.debug_view == 2) tint = float4(debug_color(i.dbg * 97u + 3u), 0.85);  // LOD colour
    else if (pc.debug_view == 3 && i.rejected != 0u) tint = float4(1.0, 0.0, 0.0, 0.85);  // occlusion-rejected -> red
    return shade_surface(*pc.scene, surf, tint);
}
