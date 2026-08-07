#pragma once

#include <cstdint>
#include <vector>

#include <string/gpu/pass_context.hpp>
#include <string/gpu/resource.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/frame_graph.hpp>

#include <string/render/geometry/geometry_scene.hpp>

namespace string::render
{

// Sorted (CPU back-to-front) transparency. Brief 20: a plain app-owned object — no base class, and
// no pass-owned buffers. What it owns is the blend/no-depth-write meshlet pipeline and the CPU list
// build; the {commands[], records[], count} block it fills is a graph resource the application
// declares.
//
// That block stays PER FRAME, and for a reason the ring verdict makes explicit: it is host-visible
// memory the CPU overwrites every frame while the GPU may still be reading last frame's contents.
// That is CPU-overwrite-vs-GPU-read pacing, which no graph edge can express, unlike the shadow ring
// (pure frames-in-flight write-after-read, which the graph derives and which therefore died).
//
// Everything else it reads is GeometryScene: the camera (sort origin), the meshlet/vertex heaps and
// DrawInfo table, and the shared mesh-cull view state. Nothing consumes what it produces, which is
// what makes it the cleanest of the seams.
class sorted_transparency
{
public:
    // The byte layout of one frame's list block, as a function of the cull capacity. Public because
    // the APPLICATION declares the buffer and this object fills it — both sides must agree on the
    // same offsets, so there is exactly one place they are computed.
    struct list_layout
    {
        VkDeviceSize commands_off = 0;   // indirect commands[] (12 B each)
        VkDeviceSize records_off = 0;    // parallel records[] ({draw_index, LOD}, 8 B each)
        VkDeviceSize count_off = 0;      // surviving-draw count word
        VkDeviceSize bytes = 0;          // total size of one frame's block
    };
    static list_layout layout_for(uint32_t max_draws);

    sorted_transparency(String::engine_context& ctx, GeometryScene* scene, VkSampleCountFlagBits samples);
    ~sorted_transparency();

    sorted_transparency(const sorted_transparency&) = delete;
    sorted_transparency& operator=(const sorted_transparency&) = delete;

    // Author onto the graph. Colour write + read-only depth put this in the same render group as the
    // opaque draws that precede it (depth-tested against the final opaque depth, no depth write,
    // blended), which is exactly the in-group position it held inside record_phase2. `list` is the
    // per-frame host-visible command/record block; `scene_data` and `stats` are the SceneData and
    // stats rings, resolved to device addresses at record time rather than latched anywhere earlier.
    void declare(::string::frame_graph& fg, ::string::gpu::image color, ::string::gpu::image depth,
                 ::string::gpu::buffer list, ::string::gpu::buffer scene_data,
                 ::string::gpu::buffer stats);

    // MSAA sample count of the scene attachments — the app declares those, so the app states this.
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;

private:
    // Collects the blend-flagged draws and computes the list layout, once the scene has geometry and
    // its DrawInfo table is mapped. Idempotent.
    void ensure();

    // Sorts the blend draws back-to-front and fills this frame's compacted command/record list on the
    // CPU, into `mapped`. Returns the surviving draw count.
    uint32_t build_list(void* mapped);

    void record(::string::pass_context& ctx, ::string::gpu::buffer list,
                ::string::gpu::buffer scene_data, ::string::gpu::buffer stats);

    ::string::gpu::device* device_ = nullptr;
    ::string::gpu::resource_allocator* allocator_ = nullptr;
    ::string::gpu::descriptor_table* descriptors_ = nullptr;
    GeometryScene* scene_ = nullptr;
    ::string::gpu::shader_program* program_ = nullptr;

    list_layout layout_{};
    std::vector<uint32_t> blend_draws_;   // draw indices flagged BLEND
    bool ready_ = false;
};

}  // namespace string::render
