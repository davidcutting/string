// Shared meshlet data model for the task/mesh geometry pipeline (brief 03). All these structs are
// read via buffer_reference (pointer), so they use Slang's NATURAL layout: float3 is 12B/4-aligned,
// float4x4 is 64B/4-aligned, pointers 8-aligned. The C++ mirrors (meshlet_data.hpp) carry
// static_asserts against the offsets emitted here; verify with:
//   slangc meshlet.slang -target spirv-asm -entry mesh_main -stage mesh -emit-spirv-directly \
//     | grep 'OpMemberDecorate %<Struct>_natural'
// NEVER trust hand-computed std430 rules for these.

// SkinVertexBuffer/PaletteBuffer (SceneData carries their pointers) — re-exported so every
// consumer of this module keeps seeing the full meshlet data model in one import.
__exported import scene_data;

// meshopt build limits (must match meshlet_data.hpp). Mesh-shader output-array sizes reference these.
static const uint kMeshletMaxVertices = 64;
static const uint kMeshletMaxTriangles = 124;

// Matches String::Vertex (tight 48B scalar layout); tangent is packed 10:10:10:2 read as a uint.
struct Vertex
{
    float3 pos;
    float3 color;
    float2 texCoord;
    float3 normal;
    uint tangent;
};
struct VertexBuffer { Vertex verts[]; };

// NORMALS DO NOT TRANSFORM LIKE POSITIONS. A direction lying ALONG the surface (a tangent) follows
// the model matrix; a direction PERPENDICULAR to it (a normal, or a meshlet's backface cone axis)
// follows the inverse transpose. They agree for rotation, uniform scale and mirroring, which is why
// using the model matrix for both went unnoticed — every scale in the test scene is uniform. Under a
// NON-uniform scale they diverge badly and in the wrong direction: stretching a surface makes it
// flatter, so its normal should tilt AWAY from the stretched axis, while the model matrix tilts it
// TOWARD. A 45-degree slope under a 4x stretch lands 62 degrees out.
//
// This applies the COFACTOR matrix, which equals det * inverse-transpose, then divides the sign of
// the determinant back out. The MAGNITUDE is irrelevant because every consumer normalizes, but the
// SIGN is not: for a mirrored transform det is negative, and the raw cofactor would point the normal
// opposite to the geometry it belongs to. The cook reverses triangle winding for mirrored draws, so
// a normal that did not also flip would disagree with the front face it is shading.
// Cofactor-with-sign costs three cross products, a dot and a select — no division and no
// singularity, both of which an explicit inverse would need.
//
// Deliberately a VECTOR function rather than one returning a float3x3. Building the matrix means
// naming its rows, and whether a float3x3 constructor fills rows or columns is exactly the kind of
// convention that is easy to get backwards and produces a plausible-looking wrong image — a
// transposed cofactor is a valid rotation, just the wrong one. Expressed as a weighted sum of the
// cofactor's COLUMNS there is nothing left to get backwards: C*n == c0'*n.x + c1'*n.y + c2'*n.z.
float3 transform_normal(float4x4 model, float3 n)
{
    // Basis vectors: the model matrix's columns (same indexing the bounds code above uses).
    const float3 c0 = float3(model[0][0], model[1][0], model[2][0]);
    const float3 c1 = float3(model[0][1], model[1][1], model[2][1]);
    const float3 c2 = float3(model[0][2], model[1][2], model[2][2]);
    // Columns of the cofactor matrix are the cross products of the other two basis vectors.
    const float3 n0 = cross(c1, c2);
    const float3 n1 = cross(c2, c0);
    const float3 n2 = cross(c0, c1);
    const float det = dot(c0, n0);
    return (n0 * n.x + n1 * n.y + n2 * n.z) * (det < 0.0 ? -1.0 : 1.0);
}

// One meshlet: a range into the meshlet-vertex remap + the packed local triangle indices, plus
// the meshopt-computed bounding sphere and quantized cone (for frustum + backface-cone culling).
struct Meshlet
{
    uint vertex_offset;    // start in the meshlet-vertex remap heap (global vertex indices)
    uint triangle_offset;  // start in the packed-triangle heap (bytes/4 = uint index)
    uint vertex_count;     // <= 64
    uint triangle_count;   // <= 124
    float3 center;         // bounding-sphere center, model space
    float radius;          // bounding-sphere radius, model space
    float3 cone_axis;      // normalized cone axis (dequantized from int8 on the CPU to float here)
    float cone_cutoff;     // cos of the cone half-angle; backface if dot(axis, view) >= cutoff
};
struct MeshletBuffer { Meshlet meshlets[]; };

// meshlet-vertices: uint remap into the shared vertex heap (global indices — keeps the existing
// HeapSuballocator/global-index scheme intact). meshlet-triangles: packed 8-bit local indices,
// u32-packed (3 bytes per triangle, read via byte extraction in the mesh shader).
struct MeshletVertexBuffer { uint indices[]; };
struct MeshletTriangleBuffer { uint packed[]; };

// The two pointers load_vertex needs to skin (from SceneData — see scene_data.slang for the
// SkinVertexBuffer/PaletteBuffer formats). Zero pointers = a pipeline that never skins (probe
// capture) or a frame with no skinned geometry; load_vertex only dereferences them behind
// di.skinned.
struct SkinContext
{
    SkinVertexBuffer *skin;
    PaletteBuffer *palette;
};

// Unpack the vertex's 10:10:10:2 tangent (matching VK_FORMAT_A2B10G10R10_SNORM_PACK32 and the
// cook's pack_tangent). Sign is the bitangent handedness (B = sign * cross(N, T)).
void unpack_tangent(uint packed, out float3 tangent, out float sign)
{
    int xi = int(packed << 22) >> 22;
    int yi = int(packed << 12) >> 22;
    int zi = int(packed << 2) >> 22;
    int wi = int(packed << 0) >> 30;
    tangent = float3(max(xi / 511.0, -1.0), max(yi / 511.0, -1.0), max(zi / 511.0, -1.0));
    sign = wi < 0 ? -1.0 : 1.0;
}

// One loaded vertex, OBJECT space — the caller applies di.model exactly as before the seam.
struct LoadedVertex
{
    float3 pos;
    float3 normal;
    float3 tangent;
    float tangent_sign;
    float2 uv;
    float3 color;
};

// Per source draw, one per LOD level: the meshlet range for that LOD and its simplification error.
struct MeshletLod
{
    uint meshlet_offset;   // first meshlet of this LOD in the global meshlet buffer
    uint meshlet_count;
    float error;           // meshopt simplification error, model-space units (0 for LOD0)
    uint _pad;
};

// Per-draw table: the transform, material slots, per-LOD meshlet ranges, a resident flag (the
// streaming gate), and a skinned flag reserved for Phase B (skinned draws inflate bounds + bypass
// cone cull). 4 LOD levels max.
struct DrawInfo
{
    float4x4 model;
    float4 base_color;
    uint base_slot;
    uint normal_slot;
    uint mr_slot;
    float metallic;
    float roughness;
    uint occlusion_slot;
    uint resident;         // 0 = not streamed in (skip), 1 = resident (draw). Streaming gate.
    uint skinned;          // reserved (Phase B): 1 = bypass cone cull, bounds pre-inflated
    float3 center;         // draw bounding-sphere center, model space (for LOD screen-error + cull)
    float radius;          // draw bounding-sphere radius, model space
    MeshletLod lods[4];
    uint lod_count;
    uint first_meshlet;    // global meshlet id of this draw's LOD0 meshlet 0 (HiZ bitfield base)
    uint total_meshlets;   // meshlets across all LODs (for the global visibility bitfield stride)
    // Signed heap delta (int32 as uint bits): meshlet-vertices hold ORIGINAL global vertex
    // indices; the vertex heap is suballocated. heap_vertex = uint(int(global) + int(this)).
    uint vertex_offset;
    // Material rendering flags (brief 04): bit 0 = cutout (alpha-test), bit 1 = double-sided
    // (bypass cone cull, rasterizer CULL_NONE via a separate command list, flip normal to viewer).
    uint flags;
    float alpha_cutoff;    // MASK threshold, sampled base-color alpha < this => clip()
    // --- Brief 23 skinning (appended; meshlet_data.hpp asserts the offsets) -----------------
    // Signed delta rebasing the ORIGINAL global vertex index into the COMPACT skin heap (the
    // vertex_offset idiom — immune to geometry streaming, which rebases only vertex_offset).
    uint skin_offset;
    uint palette_offset;   // first joint of this draw's palette in the ring, in mat4 units
    uint joint_count;      // palette length (indexing clamp + debug)
    uint _pad_skin;
};
// Brief 04 material flag bits (shared C++/Slang meaning).
static const uint kDrawFlagCutout = 1u;
static const uint kDrawFlagDoubleSided = 2u;
static const uint kDrawFlagBlend = 4u;    // alpha-blend: excluded from opaque lists, drawn sorted in the transparency pass
struct DrawInfoBuffer { DrawInfo draws[]; };

// THE one vertex-fetch seam (brief 03 promised it, brief 23 built it; 04f's shared dequant
// lands here too). `global_v` is the raw meshlet-vertex remap entry — the ORIGINAL global
// vertex index, BEFORE the streamer's heap rebase, which happens here. Returns object-space
// attributes; the CALLER applies di.model, keeping its transform arithmetic byte-identical to
// the pre-seam inline fetch (the M3a parity gate). The unused loads of a depth-only caller
// (shadow) are pure and dead-code-eliminated.
LoadedVertex load_vertex(SkinContext sc, DrawInfo di, uint global_v, VertexBuffer *vertices)
{
    uint gv = uint(int(global_v) + int(di.vertex_offset));
    Vertex v = vertices.verts[gv];
    LoadedVertex o;
    o.pos = v.pos;
    o.normal = v.normal;
    o.uv = v.texCoord;
    o.color = v.color;
    unpack_tangent(v.tangent, o.tangent, o.tangent_sign);

    // Brief 23: inline GPU skinning in the vertex pull. Blend the 4 influences' palette
    // matrices into one (weights sum to exactly 255 by cook-time construction — no
    // renormalization), then transform position AND frames by it. The frames use the plain
    // 3x3, a DELIBERATE divergence from transform_normal's cofactor: skinning matrices are
    // rotation+translation from the animation (no non-uniform scale in this content), and a
    // per-vertex cofactor costs three cross products for a case that cannot arise. The
    // caller's transform_normal(di.model, ...) for the MODEL matrix is unchanged.
    if (di.skinned != 0u && sc.skin != nullptr && sc.palette != nullptr)
    {
        uint2 sv = sc.skin.v[uint(int(global_v) + int(di.skin_offset))];
        // Accumulate with INTEGER-valued weights and divide once. Not a style choice: the
        // weights sum to exactly 255 by construction, so identical palette matrices blend to
        // exactly (M*255)/255 == M (x/x and 0/x are exact float divisions) — the identity
        // palette skins BIT-EXACTLY to the unskinned path, which is what makes the M3b parity
        // gate a binary answer. Per-influence w*(1/255) would round each term and smear mesh
        // edges by an ulp (~0.1% of pixels).
        float4x4 m = 0;
        [unroll]
        for (uint i = 0; i < 4; ++i)
        {
            uint joint = min((sv.x >> (i * 8u)) & 0xffu, di.joint_count - 1u);
            float weight = float((sv.y >> (i * 8u)) & 0xffu);
            m += sc.palette.m[di.palette_offset + joint] * weight;
        }
        m = m / 255.0;
        o.pos = mul(m, float4(o.pos, 1.0)).xyz;
        o.normal = mul((float3x3)m, o.normal);
        o.tangent = mul((float3x3)m, o.tangent);
    }
    return o;
}

// --- Brief 03b: GPU-driven draw generation --------------------------------------------------------
// The draw-cull compute (meshlet_draw_cull.slang) writes one entry per surviving draw into two
// parallel arrays + a count, then the CPU issues a single vkCmdDrawMeshTasksIndirectCountEXT. The
// task shader reads its record via the SV_DrawIndex builtin (draw index into these arrays).

// VkDrawMeshTasksIndirectCommandEXT: {groupCountX, groupCountY, groupCountZ}. 12B, matches the Vulkan
// struct exactly (indirect buffer read by the driver, NOT via buffer_reference natural layout).
struct MeshTaskCommand { uint x; uint y; uint z; };
struct MeshTaskCommandBuffer { MeshTaskCommand cmds[]; };

// Parallel to commands[]: which draw + which LOD each dispatched workgroup-set belongs to.
struct DrawRecord { uint draw_index; uint lod; };
struct DrawRecordBuffer { DrawRecord records[]; };

struct CountBuffer { uint count; };

// --- Brief 04c (resolution b): per-view scan-compacted per-draw command lists -------------------
// The per-draw dispatch fan-out (one indirect command per draw, empty for skipped draws — the 03b
// stable-slot scheme) is replaced by a two-phase cull that COMPACTS surviving draws: (1) a DRAW phase
// writes, at stable slot == draw index, each draw's surviving meshlet COUNT for this list + its
// selected LOD; (2) a COMPACTION phase runs an order-stable exclusive prefix scan over the survival
// predicate and a fill dispatch that writes, per SURVIVING draw at a dense contiguous slot in
// ascending draw order, one VkDrawMeshTasksIndirectCommandEXT (commands[]) + one {draw_index, lod}
// record (records[]), plus the surviving-draw count. One vkCmdDrawMeshTasksIndirectCountEXT then
// draws the whole list — draw count no longer drives dispatch cost, and command-index ordering pins
// coplanar depth-tie winners run-to-run (the 03b/RADV determinism constraint). The scan preserves
// input (draw) order, so submission order is deterministic (no atomics in the ordering path).

// Per-draw scratch the draw phase writes and the compaction consumes (stable slot == draw index).
struct UintBuffer { uint v[]; };

// Scan block size — one workgroup scans this many per-draw survival predicates in pass A.
static const uint kScanBlock = 256;

// --- Brief 04d: persistent per-meshlet visibility bitfield -----------------------------------------
// One bit per GLOBAL meshlet id (draw's lods[lod].meshlet_offset + local). Sized to total meshlet
// capacity, zero-initialized on creation (a cleared bit => the meshlet takes phase 2 for one frame:
// warmup = fallback = streaming/LOD-change path, all correct). Persists across frames (NOT ringed).
// Phase 1 reads a bit (visible last frame). Phase 2 sets it when the meshlet is HiZ-visible now, or
// clears it when HiZ-hidden. Set/clear are atomic (distinct meshlets may share a 32-bit word, and
// many task workgroups touch the same word in the same frame).
struct VisBitfield { uint words[]; };

bool vis_get(VisBitfield *bf, uint id)
{
    return (bf.words[id >> 5u] & (1u << (id & 31u))) != 0u;
}
void vis_set(VisBitfield *bf, uint id)
{
    uint prev;
    InterlockedOr(bf.words[id >> 5u], (1u << (id & 31u)), prev);
}
void vis_clear(VisBitfield *bf, uint id)
{
    uint prev;
    InterlockedAnd(bf.words[id >> 5u], ~(1u << (id & 31u)), prev);
}
