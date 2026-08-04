#pragma once

#include <cstdint>

#include <string/asset/geometry.hpp>

namespace string::asset
{

// ============================================================================
// Cooked scene format (brief 04b)
// ============================================================================
//
// One cooked file per source glTF; the engine's existing multi-file merge consumes N cooked
// scenes (the same rebase concept as the old flatten+merge, now over cooked tables). The format
// is little-endian raw structs with a mmap-friendly, 16-byte-aligned section table.
//
// Determinism: same source bytes -> byte-identical cooked file. Every serialized struct is
// value-initialized ({}) so padding bytes are zero; the writer never emits uninitialized memory.
//
// Layout on disk:
//   [CookedHeader]                       (fixed, at offset 0)
//   [16-byte pad to align first section]
//   <section 0 bytes> [pad to 16]
//   <section 1 bytes> [pad to 16]
//   ...
// The header holds a (offset,count,elem_size) descriptor per section; readers seek by offset.

inline constexpr char kCookedMagic[8] = { 'S', 'T', 'R', 'C', 'O', 'O', 'K', '1' };

// Bump on ANY change to the on-disk layout, a serialized struct's stride (GpuMeshlet / GpuDrawInfo /
// CookedMaterial / CookedTexture / CookedDraw / String::Vertex), or the meshlet build parameters
// (kMeshletMaxVertices/Triangles/ConeWeight/kMaxLods, LOD math). A drift here means a stale cooked
// file is silently loaded with the wrong stride -> GPU garbage; the loader re-cooks on mismatch.
// v2: vertex stream repacked grouped-per-draw (each draw's window is its own contiguous range; a v1
// split chunk windowed its parent's whole span, exploding the streamer heap to parent x chunk-count).
inline constexpr uint32_t kCookedFormatVersion = 2;

// A cooked draw: the geometry-only record the engine needs to reconstruct a GpuDrawInfo and to
// register the draw's vertex window with the streamer. Material/transform come from CookedMaterial +
// this record's transform; the meshlet LOD ranges + bounds are baked here (they become GpuDrawInfo
// lods[]/center/radius/first_meshlet/total_meshlets at load, unchanged from the old build path).
// Value-initialized on write; 16-aligned stride (matches sizeof below).
struct CookedDraw
{
    glm::mat4 transform;             // 0   world transform of the node instance (glTF space)
    glm::vec3 aabb_min;              // 64  world-space AABB (frustum + shadow + texture-LOD feedback)
    float _pad0;                     // 76
    glm::vec3 aabb_max;              // 80
    float _pad1;                     // 92
    glm::vec3 center;                // 96  meshlet-bounds center (== (aabb_min+aabb_max)/2 today)
    float radius;                    // 108
    int32_t material;                // 112 index into CookedScene.materials (-1 = none)
    uint32_t vertex_offset;          // 116 window into the shared vertex heap: min global vertex
    uint32_t vertex_count;           // 120 vertex span (vmax-vmin+1); 0 => empty draw
    uint32_t index_offset;           // 124 kept for the streamer's CPU-index-free set path parity
    uint32_t index_count;            // 128 (transient at cook; here only so the streamer window matches)
    uint32_t first_meshlet;          // 132 first meshlet in the global meshlet table (LOD0 offset)
    uint32_t total_meshlets;         // 136 total meshlets across LODs
    uint32_t lod_count;              // 140
    GpuMeshletLod lods[kMaxLods];    // 144 per-LOD {meshlet_offset, meshlet_count, error, _pad} (16B ea)
    uint32_t _pad2[2];               // 208 -> pad to 16-aligned 216
};
static_assert(sizeof(CookedDraw) == 216);
static_assert(offsetof(CookedDraw, aabb_min) == 64);
static_assert(offsetof(CookedDraw, center) == 96);
static_assert(offsetof(CookedDraw, material) == 112);
static_assert(offsetof(CookedDraw, first_meshlet) == 132);
static_assert(offsetof(CookedDraw, lods) == 144);

// glTF alpha mode, baked (mirrors GltfAlphaMode; kept independent so the cooked format doesn't
// depend on the glTF front-end).
enum class CookedAlphaMode : uint8_t { Opaque = 0, Mask = 1, Blend = 2 };

// A cooked material. Texture members index into CookedScene.textures (-1 = none). These indices are
// PER-FILE (rebased at merge, exactly like the old loader rebased material texture indices).
struct CookedMaterial
{
    glm::vec4 base_color_factor;     // 0
    float metallic_factor;           // 16
    float roughness_factor;          // 20
    int32_t base_color_texture;      // 24
    int32_t metallic_roughness_texture; // 28
    int32_t normal_texture;          // 32
    int32_t occlusion_texture;       // 36
    float alpha_cutoff;              // 40
    uint8_t alpha_mode;              // 44 (CookedAlphaMode)
    uint8_t double_sided;            // 45
    uint8_t _pad0[2];                // 46
    uint32_t _pad1;                  // 48 -> pad to 16-aligned 64
    uint32_t _pad2[3];               // 52
};
static_assert(sizeof(CookedMaterial) == 64);
static_assert(offsetof(CookedMaterial, metallic_factor) == 16);
static_assert(offsetof(CookedMaterial, alpha_cutoff) == 40);

// A cooked texture reference. The KTX2/BC7 cook is unchanged (brief 04b keeps textures as-is): the
// cooked scene just carries the texture's path (relative to the source glTF's directory, so the
// cooked file is relocatable with its assets) + srgb flag. `path` is a fixed-size inline buffer so
// the texture table stays a flat POD array (no separate string blob); paths are short.
inline constexpr uint32_t kCookedTexturePathMax = 240;
struct CookedTexture
{
    char path[kCookedTexturePathMax];  // 0   relative path (NUL-terminated, zero-padded)
    uint32_t srgb;                     // 240
    uint32_t _pad[3];                  // 244 -> 256
};
static_assert(sizeof(CookedTexture) == 256);

// Section indices in the header's section table.
enum CookedSection : uint32_t
{
    kSecVertices = 0,   // String::Vertex[]   shared vertex heap (48B each)
    kSecMeshlets,       // GpuMeshlet[]        global meshlet table (48B)
    kSecMeshletVerts,   // uint32_t[]          global meshlet-vertex remap
    kSecMeshletTris,    // uint32_t[]          packed local triangle words
    kSecDraws,          // CookedDraw[]        per-draw table
    kSecMaterials,      // CookedMaterial[]
    kSecTextures,       // CookedTexture[]
    kSecCount
};

// One section descriptor. `offset` is the byte offset of the section's first element from the start
// of the file (16-aligned). `count` * `elem_size` == the section's byte length.
struct CookedSectionDesc
{
    uint64_t offset;      // byte offset from file start (16-aligned)
    uint64_t count;       // element count
    uint32_t elem_size;   // sizeof(element) — a cheap cross-check on struct-layout drift
    uint32_t _pad;
};
static_assert(sizeof(CookedSectionDesc) == 24);

struct CookedHeader
{
    char magic[8];                             // "STRCOOK1"
    uint32_t format_version;                   // kCookedFormatVersion
    uint32_t meshlet_max_vertices;             // build params baked for a cross-check on load
    uint32_t meshlet_max_triangles;
    uint32_t max_lods;
    float meshlet_cone_weight;
    uint32_t chunk_max_meshlets;               // the chunking budget this file was cooked with (0 = off)
    uint64_t source_content_hash;              // FNV-1a of the input arrays (determinism / staleness)
    uint32_t total_meshlets;                   // == meshlets section count (convenience)
    uint32_t _pad0;
    CookedSectionDesc sections[kSecCount];     // per-section (offset,count,elem_size)
};
static_assert(offsetof(CookedHeader, sections) == 48);

}  // namespace string::asset
