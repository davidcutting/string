#pragma once

#include <string/asset/tools/gltf_loader.hpp>

namespace string::render
{

// The glTF front-end's types moved to string-asset-tools with the rest of the importer. The pass
// code consumes them (the loader hands back GltfMaterial/GltfTexture tables that the renderer binds
// into its own draw records), so re-export them unqualified rather than churning every use site.
//
// Anything the RENDERER needs long-term should migrate off these: they are the importer's shapes,
// and a second front-end (OBJ/FBX) has no reason to speak glTF's vocabulary. The cooked format is
// already the neutral hand-off; these survive only where the pass still reads front-end tables.
using ::string::asset::tools::GltfAlphaMode;
using ::string::asset::tools::GltfDraw;
using ::string::asset::tools::GltfGeometry;
using ::string::asset::tools::GltfMaterial;
using ::string::asset::tools::GltfParsed;
using ::string::asset::tools::GltfTexture;
using ::string::asset::tools::flatten_geometry;
using ::string::asset::tools::parse_gltf;

}  // namespace string::render
