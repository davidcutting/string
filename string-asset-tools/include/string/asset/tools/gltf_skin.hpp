#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <string/asset/cooked_format.hpp>
#include <string/asset/tools/gltf_loader.hpp>

namespace string::asset::tools
{

// ============================================================================
// Skin / skeleton / clip cook (brief 23)
// ============================================================================
//
// Everything ozz-offline touches during a glTF cook lives behind this seam, so bake_scene()
// stays a pure geometry core (procgen never links against animation) and bake_gltf() only
// orchestrates. The outputs split along the on-disk split: the SkinSource halves feed the
// cooked scene's v4 sections, `anim_pack` is the sibling `.anim` file's bytes.

struct SkinCook
{
    // Cooked-section inputs, shaped for bake_scene's SkinSource. skin_vertices is parallel to
    // the FLATTEN vertex order (bake Phase 2 repacks it in lockstep with the heap).
    std::vector<SkinVertex> skin_vertices;   // empty = source has no skins
    std::vector<CookedSkin> skins;           // one per glTF skin, offsets into the two below
    std::vector<glm::mat4> inverse_bind;     // concatenated, glTF skin.joints[] order
    std::vector<uint32_t> joint_remap;       // concatenated, remap[gltf_joint] = ozz joint

    // Per-skin animated AABB in character space: the union of every joint's influence sphere
    // over every sampled pose of every clip (+ bind pose). Every draw bound to skin s gets
    // this bound — deliberately shared, that is the paperdoll shape.
    std::vector<glm::vec3> skin_anim_min;
    std::vector<glm::vec3> skin_anim_max;

    // The serialized `.anim` pack (see string/anim/anim_pack.hpp). Empty when no skins.
    std::vector<uint8_t> anim_pack;
    uint32_t clip_count = 0;
};

// Extract skins + skeleton + clips from a parsed glTF, build the ozz runtime objects, and
// serialize the pack. `geometry` is the flatten output (its joints/weights arrays and the
// draws' skin indices drive quantization and the animated bounds). Returns a default SkinCook
// when the source has no skins. Throws std::runtime_error on structural errors (a skin with
// more than 256 joints, a degenerate rotation key, an ozz builder rejection).
SkinCook cook_skins(const GltfParsed& parsed, const GltfGeometry& geometry);

// The LOCKED SkinVertex quantization (normative comment in cooked_format.hpp). Exposed for
// the cook tests; joints are glTF skin-local indices, weights need not be normalized.
SkinVertex quantize_skin_vertex(const glm::u16vec4& joints, const glm::vec4& weights);

}  // namespace string::asset::tools
