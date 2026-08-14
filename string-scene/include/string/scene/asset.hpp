#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <string/asset/cooked_format.hpp>
#include <string/asset/geometry.hpp>

#include <string/scene/asset_handles.hpp>

namespace string::assets
{

// One mesh part of an asset, in registry-GLOBAL heap coordinates (already rebased at insert).
// Replaces the cooked-file-local CookedDraw + the loader's VertexWindow + the importer's GltfDraw
// meta as the runtime type. `transform` is the cooked node transform — an asset-space default
// placement (today's cook bakes world space), not a scene instance transform; the scene layer's
// instances own placement.
struct mesh_part
{
    material_id material{};       // invalid = untextured/default-material draw
    skin_id skin{};               // invalid = static geometry
    glm::mat4 transform{ 1.0f };
    glm::vec3 aabb_min{ 0.0f };
    glm::vec3 aabb_max{ 0.0f };
    glm::vec3 center{ 0.0f };
    float radius = 0.0f;
    uint32_t first_meshlet = 0;   // global meshlet heap
    uint32_t total_meshlets = 0;
    uint32_t lod_count = 0;
    ::string::asset::GpuMeshletLod lods[::string::asset::kMaxLods]{};
    uint32_t vertex_offset = 0;   // global vertex heap window
    uint32_t vertex_count = 0;
    // When skinned: skin-stream index = global vertex index + this signed delta (the compact
    // skin heap idiom from brief 23). 0 for static parts.
    int32_t skin_delta = 0;
};

// The RUNTIME material record: texture references are handles, not file-local ints, so a material
// remains re-resolvable after load (the old GltfMaterial decayed to bindless slot integers at
// upload and the indirection was gone).
struct material
{
    glm::vec4 base_color_factor{ 1.0f };
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float alpha_cutoff = 0.5f;
    texture_id base_color{};
    texture_id metallic_roughness{};
    texture_id normal{};
    texture_id occlusion{};
    ::string::asset::CookedAlphaMode alpha_mode = ::string::asset::CookedAlphaMode::Opaque;
    bool double_sided = false;
};

// A texture the registry knows about. At this stage of the migration it is the source reference
// (resolved absolute path + transfer function); the GPU pool (image, bindless slot, residency)
// moves in behind this handle next, at which point this grows the backing fields.
struct texture_desc
{
    std::filesystem::path file;   // empty = no source (consumers fall back to white)
    bool srgb = false;
};

// One skin of an asset: the palette-building inputs (windows into the registry's inverse-bind and
// joint-remap tables) plus the sibling .anim pack that animates it. skeleton_hash pairs skin and
// pack — a mismatch degrades to bind pose, never garbage (brief 23).
struct skin_binding
{
    uint64_t skeleton_hash = 0;
    uint32_t joint_count = 0;
    uint32_t ibm_offset = 0;      // window into registry inverse_bind()
    uint32_t remap_offset = 0;    // window into registry joint_remap()
    std::filesystem::path anim_pack;
};

// One loaded asset: a name plus contiguous ranges into the registry's global tables. Handed out
// only as const; the registry owns every asset and every table. (The named resource_set — the
// GPU-handle view of an asset — lands with the GPU move; the CPU ranges here are its skeleton.)
class asset
{
public:
    std::string_view name() const { return name_; }
    uint64_t content_hash() const { return content_hash_; }

    // Contiguous handle ranges into the registry's global tables.
    mesh_id first_mesh() const { return mesh_id{ first_mesh_ }; }
    uint32_t mesh_count() const { return mesh_count_; }
    material_id first_material() const { return material_id{ first_material_ }; }
    uint32_t material_count() const { return material_count_; }
    texture_id first_texture() const { return texture_id{ first_texture_ }; }
    uint32_t texture_count() const { return texture_count_; }
    skin_id first_skin() const { return skin_id{ first_skin_ }; }
    uint32_t skin_count() const { return skin_count_; }

    glm::vec3 bounds_min() const { return bounds_min_; }
    glm::vec3 bounds_max() const { return bounds_max_; }

private:
    friend class registry;
    std::string name_;
    uint64_t content_hash_ = 0;
    uint32_t first_mesh_ = 0, mesh_count_ = 0;
    uint32_t first_material_ = 0, material_count_ = 0;
    uint32_t first_texture_ = 0, texture_count_ = 0;
    uint32_t first_skin_ = 0, skin_count_ = 0;
    glm::vec3 bounds_min_{ 0.0f };
    glm::vec3 bounds_max_{ 0.0f };
};

}  // namespace string::assets
