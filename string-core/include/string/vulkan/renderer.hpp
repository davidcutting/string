#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <unordered_set>
#include <string/vulkan/frame.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/driver.hpp>
#include <string/gpu/presenter.hpp>
#include <string/gpu/queue.hpp>
#include <string/gpu/submission.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/resource_registry.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/vulkan/resource_state.hpp>
#include <string/vulkan/frame_scratch.hpp>
#include <string/vulkan/render_pass.hpp>
#include <string/vulkan/render_plan.hpp>
#include <string/vulkan/frame_graph.hpp>
#include <string/vulkan/engine_context.hpp>
#include <string/vulkan/transfer_batch.hpp>
#include <string/vulkan/passes/composite_pass.hpp>
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

namespace String
{

class Renderer
{
    static constexpr uint32_t frames_in_flight_ = 3;
    ApplicationInfo application_info_;
    std::shared_ptr<Window> window_;
    // Remappable action layer over the window's polled Input, handed to passes via engine_context.
    // Declared right after window_ so its default initializer sees an initialized window_.
    InputMap input_map_{ window_->get_input() };
    string::gpu::driver driver_;
    string::gpu::device device_;
    string::gpu::queue graphics_queue_;
    // Brief 04e M1: per-lane command + timeline infrastructure over the device's capability-derived
    // submission lanes ("main", "async-compute-N", "transfer"). Frame recording acquires recorders
    // by (lane, frame slot); the lane timelines are the ONLY per-queue timelines (frame pacing =
    // the main lane's timeline; cross-lane edges will use the others). Initialized in the ctor body.
    string::gpu::submission_set submissions_;
    uint32_t main_lane_ = 0;   // index of the "main" (graphics) lane in submissions_
    // Brief 04e M4: the async compute lane the scheduler places dependency-free compute chains
    // on (UINT32_MAX when the hardware exposes none -> inline placement, zero special cases).
    uint32_t async_lane_ = UINT32_MAX;
    // Cross-lane timeline waits the main submit must honour this frame (built in record_frame
    // when async chains were submitted, consumed by end_frame's submit).
    std::vector<VkSemaphoreSubmitInfo> async_waits_;
    string::gpu::presenter presenter_;
    string::gpu::resource_allocator allocator_;
    // Persistent upload ring on the graphics queue. Passes record their initial uploads into it
    // during construction (drained once by wait_idle before frame 0); thereafter it streams
    // per-frame uploads, flushed each frame in begin_frame. Declared after the queue/allocator it
    // borrows, so its constructor sees them initialized.
    TransferBatch transfer_batch_;
    string::gpu::descriptor_table global_descriptor_table_;
    // Brief 16 Layer 1: the resource-virtualization hub (logical image/buffer -> physical). M0 wraps
    // the allocator + descriptor_table and holds the renderer's stable render targets as `Imported`
    // handles; image_of/view_of resolve the two stable sentinels through it. Declared after the
    // allocator + descriptor_table it borrows.
    string::gpu::ResourceRegistry resources_;
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
    // Brief 16 M5: the per-frame-slot transient scratch arena is OWNED by the ResourceRegistry now
    // (the transient authority) — reach it via resources_.transients(). Passes reserve during
    // construction; the renderer materializes it after plan.build (resources_.materialize_transients).
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
    // Brief 11 step 1: the persistent frame plan. frame_passes_ are authored into a FrameGraph and
    // compiled ONCE; record_frame re-records cheaply from the cached toposort each frame instead of
    // rebuilding the graph. Recompiled only on invalidation — a per-pass enable/disable flip (the
    // enabled-signature changes) or a resize (defensive; ids are stable so the order is too). The
    // executor drives each pass through CompiledPass::source; barriers still derive from live
    // pass->usages, so per-frame-slot buffer variation needs no recompile.
    // Brief 11 endgame: the app's fluent graph author (declares each pass's I/O + flags + callbacks),
    // re-run on every recompile; the persistent FrameGraph it authors into; the compiled output the
    // executor drives (PassExec callbacks, not Pass*).
    std::function<void(FrameGraph&)> scene_author_;
    FrameGraph frame_graph_;
    CompiledFrame compiled_frame_;
    bool graph_dirty_ = true;              // force a recompile (first frame, resize)
    uint64_t enabled_signature_ = 0;       // hash of frame_passes_ enabled-states; change -> recompile
    void rebuild_execution_plan();         // author frame_passes_ -> FrameGraph -> compile -> cache
    void refresh_introspection();          // brief 14 M2: refill the debug snapshot after a compile
    // Frame pacing timeline = the main lane's timeline semaphore (submissions_ owns it). Cached
    // here because every begin/end_frame touches it.
    VkSemaphore frame_semaphore_ = VK_NULL_HANDLE;

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
    // Writes the resolved HDR target to `path` (PNG when it ends .png, else BMP). Used by the
    // single-shot r.capture.frame and the r.capture.every_n sequence (with a numbered path).
    void capture_color_target(const std::string& path);

    // Tracy GPU profiling contexts, ONE PER SUBMISSION LANE (brief 04e M1: multi-queue overlap
    // must be visible in traces), indexed like submissions_' lanes and named after them.
    // gpu_profiler_ctx_ aliases the main lane's context (the one passes receive via engine_context).
    // Null / no-op when -Dtracy is off. Created after the lane recorders are up (each context
    // needs a probe command buffer on ITS queue) and destroyed in the dtor.
    std::vector<STRING_PROFILE_GPU_CONTEXT_TYPE> lane_profiler_ctxs_;
    STRING_PROFILE_GPU_CONTEXT_TYPE gpu_profiler_ctx_ = nullptr;
    void init_gpu_profiler();

    // The main lane's recorder for a frame slot (the frame loop's command stream).
    string::gpu::command_recorder& main_recorder(uint64_t frame_slot)
    {
        return submissions_.recorder(main_lane_, static_cast<uint32_t>(frame_slot));
    }

    // Brief 06: always-on per-pass GPU timing (vkCmdWriteTimestamp2 pairs around each pass's
    // record()/record_compute()). Independent of Tracy — feeds the in-game profiler HUD and the
    // periodic [frametime] per-pass log line. Cheap; runs even when the HUD is off.
    GpuProfiler gpu_timing_;
    // Brief 14 M2: the compiled-graph snapshot the debug UI reads (via GraphIntrospect::global()).
    GraphIntrospect introspect_;
public:
    Renderer(const ApplicationInfo& application_info, std::shared_ptr<Window> window,
             const RenderPlan& plan);
    ~Renderer();

    // Read-only handle to the per-pass GPU timing so a HUD/tooling layer can render it.
    const GpuProfiler& gpu_timing() const { return gpu_timing_; }

    // Brief 11 M4 — introspection (the read seam brief 14's render-debug panel consumes). Engine-side,
    // generic, pure reads over the persistent plan. Pass timing pairs by name via gpu_timing().stats().
    // Enumerate every AUTHORED pass (incl. currently-toggled-off ones) with metadata + live enabled.
    std::vector<PassInfo> passes() const { return frame_graph_.passes(); }
    // A resource the compiled plan touches, with its topo lifetime span. `is_image` is best-effort
    // (render-target sentinels + registry image handles); the target visualizer filters on it.
    struct ResourceInfo
    {
        string::gpu::resource_id id = 0;
        bool is_image = false;
        std::optional<uint32_t> first_pass;    // first toposorted pass index that touches it
        std::optional<uint32_t> last_pass;
        std::optional<uint32_t> first_writer;  // first pass that WRITES it (nullopt = external input)
    };
    std::vector<ResourceInfo> resources() const;

    // Brief 14 M2 — the debug-UI snapshot of the compiled graph, refilled on recompile and published
    // through a process-global handle (see graph_introspect.hpp for why it is global).
    const GraphIntrospect& introspection() const { return introspect_; }

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

    // Brief 16 M0: the two STABLE render targets, held as `Imported` logical handles. COLOR_TARGET
    // resolves through color_target_ (-> color_attachment_), DEPTH_TARGET through depth_target_
    // (-> msaa_depth_). Re-pointed in handle_resize when the physical ids change. The swapchain stays
    // a special late-latched case through M0 (see acquire_swapchain / image_of).
    string::gpu::image color_target_;
    string::gpu::image depth_target_;

    // Records the whole frame's render work: groups frame_passes_ into render-pass instances by
    // their color target, derives every image barrier from the passes' declared usages (via
    // resource_states_), and transitions the swapchain to present. Replaces the old
    // begin_rendering/end_rendering scaffold.
    void record_frame();
    // Brief 16 M4 (framework-opens): open a group's dynamic-rendering instance. The framework (not the
    // passes) DERIVES every attachment's load/store/resolve from the group's lifetime facts — first
    // writer clears (else loads), last-before-resolve resolves (else stores), a declared depth-resolve
    // target min-resolves — then records vkCmdBeginRendering + viewport/scissor. Centralizes the MSAA
    // clear/load/store/resolve chain that step 2 left inline in record_frame's group loop. Barriers are
    // derived separately (in the group loop) BEFORE this call; this only opens the render pass.
    void open_group_rendering(VkCommandBuffer command_buffer, string::gpu::resource_id group_color,
        const std::optional<string::gpu::resource_id>& group_depth, string::gpu::resource_id depth_resolve,
        bool msaa_group, bool msaa_is_first, bool msaa_is_last, VkExtent2D extent,
        const VkViewport& viewport, const VkRect2D& scissor);
    // The VkImage / VkImageView backing a target string::gpu::resource_id; string::gpu::SWAPCHAIN_TARGET resolves to the
    // frame's acquired swapchain image.
    VkImage image_of(string::gpu::resource_id target) const;
    VkImageView image_view_of(string::gpu::resource_id target) const;
    // Brief 11 step 2b: aspect + mip count for a graph resource (resolving the sentinels image_of
    // maps), so the tracker transitions the correct aspect (depth vs color) and every mip of a chain
    // like the HiZ pyramid — not just mip 0 (ImageTransition defaults to a single level).
    std::pair<VkImageAspectFlags, uint32_t> image_meta(string::gpu::resource_id target) const;

    void handle_resize(const String::View::Extent& extent);

    // Binds color_attachment_ into the bindless table as a texture and hands the composite
    // pass the global set + the slot it landed in. Called at init and after each resize.
    void bind_composite_source();
};

}  // namespace String
