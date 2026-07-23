#pragma once

#include <cstdint>
#include <filesystem>

#include <string/gpu/device.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/vulkan/transfer_batch.hpp>
#include <string/platform/input.hpp>
#include <string/platform/input_map.hpp>
#include <string/core/profiler.hpp>

namespace String
{

// The GPU-side context a pass needs at construction. It bundles the renderer-owned systems
// (device, allocator, bindless table, upload recorder) plus where resources live and how many
// frames are in flight. Passes receive this as their first constructor argument so the
// application can declare *what* to draw (content) without touching *how* it's built (the GPU
// context, which only exists inside the Renderer). See render_plan.hpp for how it's injected.
struct PassContext
{
    string::gpu::device& device;
    string::gpu::resource_allocator& allocator;
    string::gpu::descriptor_table& descriptor_table;
    // Registry for hot-reloadable Slang pipelines: a pass calls create(source.slang, builder) to
    // get a shader_program whose pipeline recompiles + swaps on save (brief 01). The overlay pass
    // also reads its current_errors() to render compile diagnostics.
    string::gpu::shader_program_registry& shader_registry;
    // Batches a pass's one-time uploads (mesh/texture staging copies); flushed once by the
    // renderer after all passes are built, so uploads cost a single submit, not one per copy.
    TransferBatch& transfer;
    // Polled per-frame input (keyboard/mouse), for passes that respond to it (e.g. a camera).
    // Stable for the app's lifetime; a pass may store the reference and read it in update(). Non-
    // const so a UI pass can write intent back (e.g. request game/UI capture mode).
    Input& input;
    // Remappable action layer over `input`. Passes bind their actions here (or via a helper like
    // Camera::bind_default_controls) and query them by name, instead of reading raw key codes.
    InputMap& input_map;
    // The offscreen render targets a scene pass draws into, so it can declare its ColorWrite /
    // DepthWrite usages (the render graph orders + barriers passes from these).
    string::gpu::resource_id color_target;
    string::gpu::resource_id depth_target;
    // Root that contains shaders/ (and, at runtime, assets/); passes resolve their files here.
    std::filesystem::path resources_path;
    uint16_t frames_in_flight;
    // MSAA sample count of the scene color/depth targets. Scene-pass pipelines must set their
    // rasterizationSamples to this (via set_multisampling) to be compatible with the render pass.
    VkSampleCountFlagBits sample_count;
    // Brief 06: address of the renderer's Tracy GPU context so a pass can open finer per-STAGE GPU
    // zones INSIDE its record()/record_compute() (the renderer only wraps the whole pass in one
    // zone named by debug_name()). It is a POINTER because the context is created after the passes
    // are built (init_gpu_profiler runs post plan.build) — the pass stores this address at
    // construction and dereferences it at record time, by which point the context is live. Without
    // -Dtracy the context type is void*, so the value is always a valid null-holding slot; the GPU
    // zone macros are no-ops. May be null if the renderer chooses not to expose one.
    STRING_PROFILE_GPU_CONTEXT_TYPE* gpu_profiler_ctx;
};

}  // namespace String
