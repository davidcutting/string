#pragma once

#include <fastgltf/types.hpp>

#include <string/asset/tools/gltf_loader.hpp>

namespace string::asset::tools
{

// Holds the parsed fastgltf asset alive between parse_gltf() and its consumers, keeping fastgltf
// out of the public header. Private to the library: gltf_loader.cpp (flatten) and gltf_skin.cpp
// (skeleton/clip extraction, brief 23) both read the same parsed asset, which is why this lives
// in a src/ header rather than inside gltf_loader.cpp.
struct GltfParsed::Impl
{
    fastgltf::Asset asset;
};

}  // namespace string::asset::tools
