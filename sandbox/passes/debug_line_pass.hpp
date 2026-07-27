#pragma once

#include <memory>
#include <vector>

#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/debug_draw.hpp>

#include "meshlet_data.hpp"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <volk.h>

namespace sandbox
{

// Brief 06 debug-draw renderer. Drains the engine's immediate-mode debug ring
// (string::debug::context()) each frame into a per-frame-in-flight mapped vertex buffer and draws
// it as a world-space line list into the scene color target, AFTER geometry and BEFORE the UI (it
// shares geometry's MSAA color+depth group). Two pipelines: depth-tested (occluded by geometry) and
// overlay (always on top). The camera view_proj comes from the shared MeshOverlayStats the geometry
// pass publishes, so this pass stays decoupled from GeometryPass internals.
class DebugLinePass final : public String::Pass
{
public:
    DebugLinePass(String::engine_context& context, std::shared_ptr<const MeshOverlayStats> stats);
    ~DebugLinePass() override;

    std::string_view debug_name() const override { return "debug_line"; }

    void record(string::gpu::command_recorder& recorder, uint16_t current_frame) override;

private:
    void upload(uint16_t frame,
                std::span<const string::debug::LineVertex> depth,
                std::span<const string::debug::LineVertex> overlay);

    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    std::shared_ptr<const MeshOverlayStats> stats_;

    string::gpu::pipeline depth_pipeline_{};    // depth-tested
    string::gpu::pipeline overlay_pipeline_{};  // always on top
    VkPushConstantRange push_range_{};

    // Per-frame-in-flight host-visible vertex ring: depth verts then overlay verts, one buffer each.
    static constexpr uint32_t kMaxVerts = 1u << 18;  // 262144 line verts / frame — generous
    std::vector<string::gpu::resource_id> vertex_buffers_;
    std::vector<void*> vertex_mapped_;
    std::vector<uint32_t> depth_counts_;
    std::vector<uint32_t> overlay_counts_;
    uint32_t frames_in_flight_ = 1;
};

}  // namespace sandbox
