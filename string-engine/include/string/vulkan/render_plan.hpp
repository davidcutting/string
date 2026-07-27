#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <string/vulkan/frame_graph.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/render_pass.hpp>

namespace String
{

// The application's declaration of what to render. This is the app/library seam: the app constructs
// its (stateful) pass objects and AUTHORS the render graph fluently; the library owns the mechanics
// (when construction happens — a ready engine_context only exists inside the Renderer — plus compile,
// scheduling, sync, and re-record).
//
// The endgame shape (brief 11): the app returns a Setup — the constructed passes (which the Renderer
// owns for their lifetime, driving update()/resize()) plus a re-runnable `author(FrameGraph&)` lambda
// that declares each pass's nature FLUENTLY (reads/writes via usagesFrom, compute-only/prepass flags,
// record/prepass-compute/async callbacks, toggle). The Renderer re-runs `author` on every graph
// recompile (toggle/resize); the passes are constructed ONCE. The Pass base carries no nature
// flag-virtuals — the graph is described here, not via accreted virtual overrides.
class RenderPlan
{
public:
    struct Setup
    {
        // The constructed pass objects, in construction order. The Renderer takes ownership and drives
        // their lifecycle (update/resize/bind_color_source). The `author` lambda references them by raw
        // pointer (stable for the Renderer's lifetime).
        std::vector<std::unique_ptr<Pass>> passes;
        // Author the render graph — declares each pass fluently onto the FrameGraph. Re-run verbatim on
        // every recompile (it references the already-constructed passes), so it must not construct GPU
        // state. Runs AFTER update() each frame, so usagesFrom() sees populated per-frame usages.
        std::function<void(FrameGraph&)> author;
    };

    // The app's single configuration hook: given the ready context, construct the passes and return the
    // Setup. Called once by the Renderer.
    using ConfigureFn = std::function<Setup(engine_context&)>;

    RenderPlan& configure(ConfigureFn fn) { configure_ = std::move(fn); return *this; }

    Setup run(engine_context& ctx) const { return configure_ ? configure_(ctx) : Setup{}; }

private:
    ConfigureFn configure_;
};

}  // namespace String
