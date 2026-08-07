#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <string/application.hpp>

namespace String
{

// The set of scenes an application can render, by name.
//
// A "scene" here is exactly what an Application scene callback does: construct the app's pass
// objects, declare them onto the graph, and hand back the per-frame tick.
// That is the honest unit: the sandbox's scenes differ in WHICH PASSES EXIST (the ui-dev scene has no
// GeometryPass at all), not merely in what content those passes load, so anything narrower would not
// describe them.
//
// The registry itself is deliberately dumb: a name, something to show a human, and the configure
// function. It knows nothing about GPU state, loading, or switching — the Renderer owns that. What it
// buys is that the list of scenes becomes DATA rather than a chain of string comparisons buried in
// the app's plan builder, which is what lets a UI panel enumerate them and lets `STRING_SCENE` become
// a lookup instead of a branch.
//
// LAYERING: this lives beside Application rather than in the sandbox because two different consumers
// need it — the app registers scenes, and the debug UI lists them. Putting it in the app would mean
// the debug library reaching up into the application to enumerate them.
class SceneRegistry
{
public:
    struct Scene
    {
        std::string name;           // stable id, matches STRING_SCENE / dbg.scene
        std::string description;    // one line, for the UI
        // Brief 20: a scene is now the app's single construct-and-declare callback (see Application),
        // not a RenderPlan the renderer drove.
        Application::scene_fn configure;
        // Source assets this scene loads, when it has any (content scenes and sponza). Empty for
        // scenes with nothing on disk behind them (ui, lookdev). Used by the texture cook.
        std::vector<std::filesystem::path> assets;
        // Discovered by scanning the user's content folder, rather than registered in code. Only
        // these are dropped by a rescan — built-ins (ui, lookdev, sponza) must survive one.
        bool from_content = false;
    };

    static SceneRegistry& instance();

    // Registration order is preserved — it is the order a UI lists them in, so the app controls it.
    // Re-registering a name REPLACES it, so a scene can be overridden without unregistering first.
    void add(std::string name, std::string description, Application::scene_fn configure,
             std::vector<std::filesystem::path> assets = {}, bool from_content = false);

    // --- rescan ----------------------------------------------------------------------------------
    // Re-run content discovery so an asset dropped in while the engine is running shows up without a
    // restart. The registry cannot scan anything itself (it deliberately knows nothing about assets),
    // so the app installs the scanner and the UI just asks for it.
    using RescanFn = std::function<void()>;
    void set_rescan(RescanFn fn) { rescan_ = std::move(fn); }
    [[nodiscard]] bool can_rescan() const noexcept { return static_cast<bool>(rescan_); }

    // --- texture cook ----------------------------------------------------------------------------
    // Cook a scene's textures to KTX2/BC7 so later loads stream blocks instead of decoding PNGs.
    // Installed by the app for the same layering reason as the scanner: the registry knows nothing
    // about assets or cooking. BLOCKING and slow (minutes) — see cook_scene_textures_for.
    using CookFn = std::function<void(const Scene&)>;
    void set_cook(CookFn fn) { cook_ = std::move(fn); }
    [[nodiscard]] bool can_cook() const noexcept { return static_cast<bool>(cook_); }
    // No-op when the name is unknown, no cooker is installed, or the scene has no source assets
    // (ui/lookdev/shader scenes) — asking to cook those is not an error, there is just nothing to do.
    void cook(std::string_view name);
    // Drops content scenes and re-runs the scanner. The ACTIVE scene is kept whatever happens —
    // dropping the configure function of the scene currently on screen would strand a later reload.
    void rescan();

    [[nodiscard]] const std::vector<Scene>& scenes() const noexcept { return scenes_; }
    // Null when absent. The caller decides what a missing scene means — the sandbox falls back to its
    // default rather than failing, because a typo'd STRING_SCENE should not be fatal.
    [[nodiscard]] const Scene* find(std::string_view name) const noexcept;

    // The scene currently loaded, "" before the first load. Set by whoever performs the load.
    [[nodiscard]] std::string_view active() const noexcept { return active_; }
    void set_active(std::string_view name) { active_ = name; }

    // Ask for a different scene. A REQUEST rather than a call because the caller is typically a UI
    // click handler running mid-frame, and loading destroys the very passes that are being recorded;
    // the frame loop takes the request at a safe point. Requesting the active scene is a no-op, so a
    // click on the current entry costs nothing rather than reloading it.
    void request(std::string_view name)
    {
        if (name != active_ && find(name) != nullptr) pending_ = name;
    }
    [[nodiscard]] bool has_pending() const noexcept { return !pending_.empty(); }
    // Returns the requested scene and clears the request. Null if there was none.
    const Scene* take_pending()
    {
        if (pending_.empty()) return nullptr;
        const Scene* s = find(pending_);
        pending_.clear();
        return s;
    }

private:
    std::vector<Scene> scenes_;
    std::string active_;
    std::string pending_;
    RescanFn rescan_;
    CookFn cook_;
};

}  // namespace String
