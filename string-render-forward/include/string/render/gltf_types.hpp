#pragma once

#include <string/asset/tools/gltf_loader.hpp>

namespace string::render
{

// Unqualified re-exports of the importer's types, which live in string-asset-tools. Pass code still
// reads front-end tables (GltfMaterial/GltfTexture) when binding its draw records.
//
// The renderer should migrate off these: they are the importer's shapes, and a second front-end
// (OBJ/FBX) has no reason to speak glTF's vocabulary. The cooked format is the neutral hand-off.
using ::string::asset::tools::GltfAlphaMode;
using ::string::asset::tools::GltfDraw;
using ::string::asset::tools::GltfGeometry;
using ::string::asset::tools::GltfMaterial;
using ::string::asset::tools::GltfParsed;
using ::string::asset::tools::GltfTexture;
using ::string::asset::tools::flatten_geometry;
using ::string::asset::tools::parse_gltf;

}  // namespace string::render
