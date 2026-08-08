#pragma once

#include <entt/entt.hpp>
#include <filesystem>
#include <string/gpu/resource_allocator.hpp>

namespace string
{

class Scene
{
    entt::registry registry_;
public:
    Scene() = default;
    ~Scene() = default;

    void load_from_disk(string::gpu::resource_allocator& resource_allocator, const std::filesystem::path& path);
};

}