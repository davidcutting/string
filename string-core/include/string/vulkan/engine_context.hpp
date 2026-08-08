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

namespace string
{

// The GPU-side services a pass object needs at CONSTRUCTION (the reference doc has no analogue —
// Daxa's app owns its device directly). Distinct from `string::pass_context`, which is the
// EXECUTE-time surface and is where handles resolve.
//
// Brief 20: this no longer carries `color_target` / `depth_target`. Those were raw resource_id
// sentinels a pass declared its attachment writes against; the scene's colour and depth are now
// viewport-scaled transients the application declares to its frame_graph and passes to whichever
// pass objects need them, as logical handles. It also no longer carries a ResourceRegistry — the
// graph is the resolution authority.
struct engine_context
{
    string::gpu::device& device;
    string::gpu::resource_allocator& allocator;
    string::gpu::descriptor_table& descriptor_table;
    // Registry for hot-reloadable Slang pipelines: a pass calls create(source.slang, builder) to get
    // a shader_program whose pipeline recompiles + swaps on save.
    string::gpu::shader_program_registry& shader_registry;
    // Batches a pass's one-time construction uploads, flushed once after all passes are built so
    // uploads cost a single submit rather than one per copy.
    TransferBatch& transfer;
    // Polled per-frame input. Non-const so a UI pass can write intent back (e.g. capture mode).
    Input& input;
    // Remappable action layer over `input`; passes bind actions here instead of raw key codes.
    InputMap& input_map;
    // Root containing shaders/ (and, at runtime, assets/); passes resolve their files here.
    std::filesystem::path resources_path;
    uint16_t frames_in_flight;
    // Address of the renderer's Tracy GPU context, so a pass can open finer per-stage GPU zones
    // inside its recording callback. A POINTER because the context is created after the passes are
    // built — the pass stores the address and dereferences it at record time, by which point the
    // context is live. Without -Dtracy the type is void* and the zone macros are no-ops.
    STRING_PROFILE_GPU_CONTEXT_TYPE* gpu_profiler_ctx;
};

}  // namespace string
