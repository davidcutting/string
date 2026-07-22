#pragma once

#include <memory>
#include <vector>
#include <unordered_set>
#include <string/vulkan/frame.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/driver.hpp>
#include <string/gpu/presenter.hpp>
#include <string/gpu/queue.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/vulkan/resource_state.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/render_plan.hpp>
#include <string/vulkan/pass_context.hpp>
#include <string/vulkan/transfer_batch.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
#include <string/platform/input_map.hpp>
#include <string/core/job_system.hpp>
#include <string/core/file_watch_service.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>

#include <volk.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/platform/window.hpp>
#include <string/gpu/device.hpp>
#include <string/core/app_info.hpp>

namespace String
{

class Renderer
{
    static constexpr uint32_t frames_in_flight_ = 3;
    ApplicationInfo application_info_;
    std::shared_ptr<Window> window_;
    // Remappable action layer over the window's polled Input, handed to passes via PassContext.
    // Declared right after window_ so its default initializer sees an initialized window_.
    InputMap input_map_{ window_->get_input() };
    string::gpu::driver driver_;
    string::gpu::device device_;
    string::gpu::queue graphics_queue_;
    string::gpu::queue compute_queue_;
    string::gpu::presenter presenter_;
    string::gpu::resource_allocator allocator_;
    // Persistent upload ring on the graphics queue. Passes record their initial uploads into it
    // during construction (drained once by wait_idle before frame 0); thereafter it streams
    // per-frame uploads, flushed each frame in begin_frame. Declared after the queue/allocator it
    // borrows, so its constructor sees them initialized.
    TransferBatch transfer_batch_;
    string::gpu::descriptor_table global_descriptor_table_;
    // Shader hot-reload plumbing (brief 01). shader_jobs_ is a small pool for off-thread mtime
    // scans + Slang recompiles; the watcher polls it each frame; the compiler is the in-process
    // Slang session (SPIR-V + reflection with a content-hash disk cache); the registry owns every
    // reloadable pipeline and swaps rebuilt ones at the frame boundary. Declared before the passes
    // (composite + scene) so they can register their pipelines during construction.
    string::core::job_system shader_jobs_;
    string::core::file_watch_service file_watcher_;
    string::gpu::shader_compiler shader_compiler_;
    string::gpu::shader_program_registry shader_registry_;
    // Derives the frame's image-layout barriers from tracked state (see begin/end_rendering).
    ResourceStateTracker resource_states_;
    CompositePass composite_pass_;
    // Ordered passes that draw into the offscreen HDR target (color_attachment_), recorded
    // between begin_rendering() and end_rendering(). composite_pass_ is the fixed resolve
    // (offscreen -> swapchain) and is intentionally NOT in this list. Built in the ctor body
    // from the application's RenderPlan, once the allocator/table/transfer recorder are ready.
    std::vector<std::unique_ptr<Pass>> scene_passes_;
    // The frame graph's execution list: scene_passes_ then composite_pass_, recorded as
    // render-pass groups by record_frame(). written_resources_ = the resources the graph itself
    // writes (color/depth/swapchain) — static uploaded inputs (textures/buffers) are never
    // re-transitioned.
    std::vector<Pass*> frame_passes_;
    std::unordered_set<string::gpu::resource_id> written_resources_;
    VkSemaphore frame_semaphore_;

    // Swapchain image acquired at the start of the frame (in begin_frame), so that
    // end_rendering has a valid blit target and end_frame can submit/present against it.
    VkImage acquired_image_ = VK_NULL_HANDLE;
    VkImageView acquired_image_view_ = VK_NULL_HANDLE;
    VkSemaphore acquired_wait_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore acquired_signal_semaphore_ = VK_NULL_HANDLE;
    uint64_t frame_count_ = 1;
    uint64_t current_frame_ = 0;
    std::array<Frame, frames_in_flight_> frames_;

    // Debug frame capture (env STRING_CAPTURE_FRAME=N [+ STRING_CAPTURE_PATH]): after frame N is
    // submitted, waits idle and writes the resolved HDR color target to a BMP so a headless
    // agent/tool can inspect real output. 0 = disabled.
    uint64_t capture_frame_ = 0;
    std::string capture_path_;
    void capture_color_target();
public:
    Renderer(const ApplicationInfo& application_info, std::shared_ptr<Window> window,
             const RenderPlan& plan);
    ~Renderer();

    void update();

    void begin_frame();
    void draw();
    void end_frame();

private:
    // MSAA: scene passes render into the multisampled color + depth targets, and the color is
    // resolved into color_attachment_ (1-sample), which the composite pass samples.
    static constexpr VkSampleCountFlagBits msaa_samples_ = VK_SAMPLE_COUNT_4_BIT;
    string::gpu::resource_id color_attachment_;   // 1-sample resolve target (sampled by composite)
    string::gpu::resource_id msaa_color_;         // multisampled scene color target
    string::gpu::resource_id msaa_depth_;         // multisampled scene depth target

    // Records the whole frame's render work: groups frame_passes_ into render-pass instances by
    // their color target, derives every image barrier from the passes' declared usages (via
    // resource_states_), and transitions the swapchain to present. Replaces the old
    // begin_rendering/end_rendering scaffold.
    void record_frame();
    // The VkImage / VkImageView backing a target string::gpu::resource_id; string::gpu::SWAPCHAIN_TARGET resolves to the
    // frame's acquired swapchain image.
    VkImage image_of(string::gpu::resource_id target) const;
    VkImageView image_view_of(string::gpu::resource_id target) const;

    void handle_resize(const String::View::Extent& extent);

    // Binds color_attachment_ into the bindless table as a texture and hands the composite
    // pass the global set + the slot it landed in. Called at init and after each resize.
    void bind_composite_source();
};

}  // namespace String
