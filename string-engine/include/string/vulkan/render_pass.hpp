#pragma once

#include <string/gpu/command_recorder.hpp>
#include <string/vulkan/resource_usage.hpp>

#include <vector>

#include <volk.h>

namespace String
{

// Abstract base for a recordable render pass. It owns no GPU objects — each pass manages its
// own pipeline/descriptors as members (a `string::gpu::pipeline` member, by convention). The base is just
// the interface plus the current screen size and the typed resource usages a future render
// graph will use to order passes and derive barriers.
struct Pass
{
    VkExtent2D screen_size{};

    // The resources this pass reads/writes and how (Access + stage). Reads and writes share
    // one list; is_write(usage.access) distinguishes them. Not yet consumed — the forward hook
    // the render graph will build execution order + barriers from.
    std::vector<ResourceUsage> usages;

    virtual ~Pass() = 0;
    // Per-frame CPU update. Defaults to a no-op so passes that don't need one (grid, most
    // static passes) can skip it; record() is the only method a pass must implement.
    virtual void update(float /*delta_time*/, uint16_t /*current_frame*/) {}
    // Optional compute prepass, recorded *outside* dynamic rendering before the graphics groups
    // (e.g. GPU culling that writes an indirect buffer the pass's record() then draws). Returns
    // true if it recorded any compute work, so the renderer knows to barrier compute->draw.
    virtual bool record_compute(string::gpu::command_recorder& /*recorder*/, uint16_t /*current_frame*/) { return false; }
    virtual void record(string::gpu::command_recorder& recorder, uint16_t current_frame) = 0;
    void resize(VkExtent2D extent) { screen_size = extent; }
};

// A pure-virtual destructor still needs a definition so derived passes can link.
inline Pass::~Pass() = default;

} // namespace String