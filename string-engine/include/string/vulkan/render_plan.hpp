#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <string/vulkan/pass_context.hpp>
#include <string/vulkan/render_pass.hpp>

namespace String
{

// The application's declaration of what to render: an ordered list of passes, described by
// *type + content* rather than constructed up front. The GPU resources a pass needs
// (PassContext) only exist inside the Renderer, so construction is deferred — the plan stores
// factories that the Renderer runs once, in order, when its context is ready.
//
// This is the app/library seam. The application owns the plan (which passes, the mesh, the UI
// layout); the library owns the mechanics (when and with what GPU context they're built, and
// how they're recorded). It is deliberately graph-ready: each constructed Pass carries its
// reads/writes, so a future dependency-graph layer can toposort `build()`'s output before
// recording — without changing this app-facing API.
class RenderPlan
{
public:
    using Factory = std::function<std::unique_ptr<Pass>(PassContext&)>;

    // Declare a pass of type P, forwarding `args` as its content (everything after the
    // PassContext, which the Renderer injects). Order of add() calls is record order.
    template <typename P, typename... Args>
    RenderPlan& add(Args... args)
    {
        factories_.push_back([args...](PassContext& ctx) -> std::unique_ptr<Pass> {
            return std::make_unique<P>(ctx, args...);
        });
        return *this;
    }

    // Construct every declared pass against the ready GPU context, preserving order.
    // Called by the Renderer; the application never sees a PassContext.
    std::vector<std::unique_ptr<Pass>> build(PassContext& ctx) const
    {
        std::vector<std::unique_ptr<Pass>> passes;
        passes.reserve(factories_.size());
        for (const auto& factory : factories_)
            passes.push_back(factory(ctx));
        return passes;
    }

private:
    std::vector<Factory> factories_;
};

}  // namespace String
