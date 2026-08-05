#include <algorithm>

#include <string/vulkan/scene_registry.hpp>

namespace String
{

SceneRegistry& SceneRegistry::instance()
{
    static SceneRegistry registry;
    return registry;
}

void SceneRegistry::add(std::string name, std::string description, RenderPlan::ConfigureFn configure,
                        std::vector<std::filesystem::path> assets, bool from_content)
{
    const auto it = std::find_if(scenes_.begin(), scenes_.end(),
                                 [&](const Scene& s) { return s.name == name; });
    if (it != scenes_.end())
    {
        it->description = std::move(description);
        it->configure = std::move(configure);
        it->assets = std::move(assets);
        it->from_content = from_content;
        return;
    }
    scenes_.push_back(Scene{ std::move(name), std::move(description), std::move(configure),
                             std::move(assets), from_content });
}

void SceneRegistry::cook(std::string_view name)
{
    if (!cook_)
    {
        return;
    }
    const Scene* s = find(name);
    if (s == nullptr || s->assets.empty())
    {
        return;   // nothing on disk behind this scene — not an error
    }
    cook_(*s);
}

void SceneRegistry::rescan()
{
    if (!rescan_)
    {
        return;
    }
    // Keep the active scene even if its file vanished: it is on screen, and dropping its configure
    // function would strand any later reload of it. A stale entry is a far smaller problem than a
    // dangling active scene.
    std::erase_if(scenes_, [this](const Scene& s) {
        return s.from_content && s.name != active_;
    });
    rescan_();
}

const SceneRegistry::Scene* SceneRegistry::find(std::string_view name) const noexcept
{
    const auto it = std::find_if(scenes_.begin(), scenes_.end(),
                                 [&](const Scene& s) { return s.name == name; });
    return it == scenes_.end() ? nullptr : &*it;
}

}  // namespace String
