#pragma once

#include <entt/entt.hpp>
#include <filesystem>
#include <string/vulkan/resource_allocator.hpp>

namespace String
{

class Scene
{
    entt::registry registry_;
public:
    Scene() = default;
    ~Scene() = default;

    void load_from_disk(ResourceAllocator& resource_allocator, const std::filesystem::path& path);
};

}