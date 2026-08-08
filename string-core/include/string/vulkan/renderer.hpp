#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <string/vulkan/frame.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/driver.hpp>
#include <string/gpu/presenter.hpp>
#include <string/gpu/queue.hpp>
#include <string/gpu/submission.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/transfer_batch.hpp>
#include <string/vulkan/gpu_profiler.hpp>
#include <string/vulkan/graph_introspect.hpp>
#include <string/platform/input_map.hpp>
#include <string/core/job_system.hpp>
#include <string/core/file_watch_service.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/core/profiler.hpp>

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

namespace string
{

// Brief 20 — the renderer owns what is DERIVED from the graph, never what is declared in it.
//
// It does not own passes, does not own a graph, and does not author anything. The application
// constructs a `frame_graph`, declares its passes into it once, compiles once, and hands the
// resulting `compiled_frame` to render_frame() every frame.
//
// What stays here is the frame machinery a declaration cannot express: swapchain acquire, the
// per-slot timeline pacing, lane submission with its cross-queue edges, present, resize, and the
// build-time services passes are constructed from (engine_context).
class renderer
{
public:
    renderer(const string::ApplicationInfo& application_info, std::shared_ptr<string::Window> window);
    ~renderer();

    // The build-time services an application's pass objects are constructed from. Stable for the
    // renderer's lifetime.
    string::engine_context& context() { return context_; }

    // The acquired swapchain image, declared by the app as a persistent with `swapchain = true`.
    // Its backing is late-latched per frame; the graph substitutes it at execute.
    static constexpr gpu::resource_id swapchain_target = gpu::SWAPCHAIN_TARGET;

    // Run one frame: wait out the slot, acquire, poll shader reloads, flush pending uploads,
    // execute the compiled graph, submit across lanes and present. `dt` is the app's timestep.
    void render_frame(compiled_frame& frame, float dt);

    // The window was resized. Recreates the swapchain and hands the new extent to the graph, which
    // re-sizes its viewport-scaled transients. No re-authoring and no recompile.
    void resize(compiled_frame& frame, const string::View::Extent& extent);

    VkExtent2D extent() const { return presenter_.get_extent(); }

    // The frame slot the NEXT render_frame will record into. Per-frame CPU work must pack its rings
    // against this same slot — packing slot 0 while recording slot N means most frames draw from a
    // ring nothing filled, which shows up as flicker rather than as an error.
    std::uint32_t frame_slot() const { return static_cast<std::uint32_t>(current_frame_); }

    // Block until every submitted frame has finished. The application must call this BEFORE it
    // destroys its pass objects: those destructors free pipelines and images the in-flight command
    // buffers still refer to, and nothing else in the shutdown path waits.
    void wait_idle();
    // The REAL swapchain format. A pass that renders to it must build its pipeline against this, not
    // against a guess — a mismatch is a validation error at first draw and nothing before it.
    VkFormat swapchain_format() const { return presenter_.get_format(); }

    const string::GpuProfiler& gpu_timing() const { return gpu_timing_; }
    const string::GraphIntrospect& introspection() const { return introspect_; }

    // Record and submit everything the SCENE staged while it was being built (vertex/meshlet heaps,
    // the CPU-decoded textures, the UI atlas), and wait for it. The one acceptable wait: it runs once,
    // after construction and before the first frame, and it exists to bound peak staging — otherwise
    // a scene's whole upload set sits in host memory until frame 1's uploads pass records it.
    // Per-FRAME uploads never come here; they are a declared pass inside the graph.
    void flush_construction_uploads();

    // Refill the debug snapshot from a compiled graph. Called by the app after it compiles.
    void publish_introspection(const compiled_frame& frame);

    // Declare the headless capture onto the app's graph: which logical image it reads, and the pass
    // that copies it. The transitions around that copy are DERIVED from the declaration, which is
    // what retired the pair of hand-written ones that had to assume a layout.
    void declare_capture(frame_graph& fg, gpu::image source);

    // The OWNER half of a resize (brief 21 D3): the app re-backs its viewport-sized persistents
    // (the hiz depth-history ring) and re-points their handles via set_images. Called by resize()
    // under device idle, BEFORE the graph half re-materializes and re-binds — so the rebind picks
    // up the new backing. Declarations never change; this is a backing swap.
    void on_resize(std::function<void(VkExtent2D)> cb) { resize_cb_ = std::move(cb); }

    // Write the frame's presented image to `path` (PNG when it ends .png, else BMP), for the
    // headless capture gates.
    void capture(const std::string& path);

private:
    static constexpr uint32_t frames_in_flight_ = 3;
    string::ApplicationInfo application_info_;
    std::shared_ptr<string::Window> window_;
    string::InputMap input_map_{ window_->get_input() };
    gpu::driver driver_;
    gpu::device device_;
    gpu::queue graphics_queue_;
    // Per-lane command + timeline infrastructure over the device's capability-derived submission
    // lanes ("main", "async-compute-N", "transfer"). The lane timelines are the only per-queue
    // timelines; frame pacing is the main lane's.
    gpu::submission_set submissions_;
    uint32_t main_lane_ = 0;
    // The async compute lane the graph places `pass_lane::async` passes on (UINT32_MAX when the
    // hardware exposes none — those passes then record inline, same graph, serialized placement).
    uint32_t async_lane_ = UINT32_MAX;
    std::vector<VkSemaphoreSubmitInfo> async_waits_;
    gpu::presenter presenter_;
    gpu::resource_allocator allocator_;
    // Stages uploads and hands them to the declared uploads pass; owns no queue and no timeline.
    string::TransferBatch transfer_batch_;
    gpu::descriptor_table global_descriptor_table_;
    // Shader hot-reload plumbing: an off-thread mtime scan + Slang recompile pool, the watcher the
    // frame polls, the in-process compiler, and the registry that swaps rebuilt pipelines at the
    // frame boundary.
    core::job_system shader_jobs_;
    core::file_watch_service file_watcher_;
    gpu::shader_compiler shader_compiler_;
    gpu::shader_program_registry shader_registry_;
    string::engine_context context_;

    VkSemaphore frame_semaphore_ = VK_NULL_HANDLE;

    VkImage acquired_image_ = VK_NULL_HANDLE;
    VkImageView acquired_image_view_ = VK_NULL_HANDLE;
    VkSemaphore acquired_wait_semaphore_ = VK_NULL_HANDLE;
    VkSemaphore acquired_signal_semaphore_ = VK_NULL_HANDLE;
    uint64_t frame_count_ = 1;
    uint64_t current_frame_ = 0;
    std::array<string::Frame, frames_in_flight_> frames_;

    // Tracy GPU contexts, one per submission lane so multi-queue overlap is visible in traces.
    std::vector<STRING_PROFILE_GPU_CONTEXT_TYPE> lane_profiler_ctxs_;
    STRING_PROFILE_GPU_CONTEXT_TYPE gpu_profiler_ctx_ = nullptr;
    void init_gpu_profiler();

    gpu::command_recorder& main_recorder(uint64_t frame_slot)
    {
        return submissions_.recorder(main_lane_, static_cast<uint32_t>(frame_slot));
    }

    // Always-on per-pass GPU timing feeding the in-game profiler HUD and the periodic
    // [frametime] log line. Independent of Tracy.
    gpu::image capture_source_{};
    // Filled by the declared capture pass; read (and written to disk) after that frame retires.
    gpu::resource_id capture_staging_ = 0;
    VkDeviceSize capture_staging_bytes_ = 0;
    bool capture_armed() const;
    void record_capture(pass_context& ctx);
    std::function<void(VkExtent2D)> resize_cb_;
    compiled_frame* capture_frame_ = nullptr;
    string::GpuProfiler gpu_timing_;
    string::GraphIntrospect introspect_;

    void submit_lane(uint32_t lane, std::uint32_t slot, uint64_t value);
    void begin_frame();
    void end_frame();
};

}  // namespace string
