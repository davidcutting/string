#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <filesystem>

namespace String
{

enum class ShaderStage : std::uint8_t
{
    UNKNOWN,
    COMPUTE,
    TASK,
    MESH,
    GEOMETRY,
    VERTEX,
    FRAGMENT,
    TESSELLATION_CONTROL,
    TESSELLATION_EVALUATION
};

struct ShaderMetaInfo
{
    std::filesystem::path file_path;
    std::vector<std::filesystem::path> dependencies;
    uint64_t file_hash;
    ShaderStage stage;
};

struct CompilationResult
{
    ShaderMetaInfo shader_meta_info;
    std::span<std::byte> spirv_bytecode;
    std::string error_message;
    bool success;
};

struct CompilationTask
{
    ShaderMetaInfo shader_meta_info;
    std::filesystem::path shader_path;
    bool is_dependency_change;
};

}