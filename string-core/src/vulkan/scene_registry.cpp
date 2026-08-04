#include <algorithm>

#include <string/vulkan/scene_registry.hpp>

namespace String
{

SceneRegistry& SceneRegistry::instance()
{
    static SceneRegistry registry;
    return registry;
}

void SceneRegistry::add(std::string name, std::string description, RenderPlan::ConfigureFn configure)
{
    const auto it = std::find_if(scenes_.begin(), scenes_.end(),
                                 [&](const Scene& s) { return s.name == name; });
    if (it != scenes_.end())
    {
        it->description = std::move(description);
        it->configure = std::move(configure);
        return;
    }
    scenes_.push_back(Scene{ std::move(name), std::move(description), std::move(configure) });
}

const SceneRegistry::Scene* SceneRegistry::find(std::string_view name) const noexcept
{
    const auto it = std::find_if(scenes_.begin(), scenes_.end(),
                                 [&](const Scene& s) { return s.name == name; });
    return it == scenes_.end() ? nullptr : &*it;
}

}  // namespace String
