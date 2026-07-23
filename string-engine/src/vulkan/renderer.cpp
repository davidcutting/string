#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <string/core/cache_dir.hpp>
#include <string/core/cvar.hpp>
#include <string/core/png_writer.hpp>
#include <string/debug_draw.hpp>
#include <string/gpu/driver.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/render_graph.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/presenter.hpp>
#include <string/core/platform_detection.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/queue.hpp>
#include <string/gpu/descriptor_allocator.hpp>

#include <volk.h>

namespace String
{

// Frame-capture levers, now CVar-backed. The env bridge honours the canonical STRING_R_CAPTURE_*
// names, and the aliases keep the legacy STRING_CAPTURE_FRAME / STRING_CAPTURE_PATH names working
// verbatim (the alias "capture_frame" maps to STRING_CAPTURE_FRAME via env_name_for). This is the
// engine-side proof-of-use for the CVar system; sandbox env-lever migration is a follow-up.
namespace
{
string::core::CVar<int32_t>& capture_frame_cvar()
{
    static string::core::CVar<int32_t> v{
        "r.capture.frame", 0,
        "capture the resolved HDR target after this frame index then stop (0 = disabled)"};
    static const bool aliased = [] { v.add_alias("capture_frame"); return true; }();
    (void)aliased;
    return v;
}
string::core::CVar<std::string>& capture_path_cvar()
{
    static string::core::CVar<std::string> v{
        "r.capture.path", "/tmp/string_capture.bmp", "output path for r.capture.frame"};
    static const bool aliased = [] { v.add_alias("capture_path"); return true; }();
    (void)aliased;
    return v;
}
// Brief 06: capture-sequence. When >0, capture every Nth frame to a numbered file derived from
// r.capture.path (e.g. /tmp/cap.png -> /tmp/cap_00042.png). Runs alongside the single-shot
// r.capture.frame; independent lever, honoured live from the console.
string::core::CVar<int32_t>& capture_every_n_cvar()
{
    static string::core::CVar<int32_t> v{
        "r.capture.every_n", 0,
        "capture every Nth frame to numbered files derived from r.capture.path (0 = off)"};
    static const bool aliased = [] { v.add_alias("capture_every_n"); return true; }();
    (void)aliased;
    return v;
}

// Insert a zero-padded frame index before the extension: "/a/b.png" + 42 -> "/a/b_00042.png".
std::string numbered_capture_path(const std::string& base, uint64_t frame)
{
    const std::size_t dot = base.find_last_of('.');
    const std::size_t slash = base.find_last_of("/\\");
    const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    char idx[16];
    std::snprintf(idx, sizeof(idx), "_%05llu", static_cast<unsigned long long>(frame));
    if (has_ext) return base.substr(0, dot) + idx + base.substr(dot);
    return base + idx;
}

// Brief 04e M4: async-compute placement lever (alias STRING_ASYNC). Default on; off = the same
// graph records the async chains inline on the main queue (the 1-lane degrade path) — the A/B
// lever agents use to isolate cross-queue sync from placement.
string::core::CVar<bool>& async_enabled_cvar()
{
    static string::core::CVar<bool> v{"r.async.enabled", true,
        "place dependency-free compute chains on the async compute lane (0 = record inline)"};
    static const bool aliased = [] { v.add_alias("async"); return true; }();
    (void)aliased;
    return v;
}
}  // namespace

Renderer::Renderer(const ApplicationInfo& application_info, std::shared_ptr<Window> window,
                   const RenderPlan& plan)
: application_info_(application_info)
, window_(std::move(window))
, driver_(application_info, window_)
, device_(driver_, window_)
, graphics_queue_(device_.get_queue(string::gpu::queue_type::GRAPHICS))
, presenter_(device_, window_, frames_in_flight_)
, allocator_({ driver_.get_instance(), device_.get_physical_device(), device_.get_device() })
, transfer_batch_(device_, allocator_, graphics_queue_)
, global_descriptor_table_(device_.get_device(), allocator_)
// Shader hot-reload: a 1-thread pool (mtime scans + recompiles are light and serial), the watcher
// polled each frame, and the Slang compiler with a content-hash cache under the shaders dir. The
// shaders/ tree is the include/import search root.
, shader_jobs_(1)
, file_watcher_(shader_jobs_)
// Cache lives under the per-user cache dir (the resources/shaders tree may be read-only, e.g.
// the Nix store); it is content-hash keyed so a stale entry is simply never hit.
, shader_compiler_(
    string::core::user_cache_dir("shaders"),
    { std::filesystem::path(application_info.resources_directory) / "shaders" })
, shader_registry_(device_, shader_compiler_, file_watcher_, shader_jobs_)
, composite_pass_(device_, std::filesystem::path(application_info.resources_directory), global_descriptor_table_.get_layout(), presenter_.get_format(), shader_registry_)
{
    STRING_LOG_DEBUG("Initializing renderer...");
    const auto resources_path = std::filesystem::path(application_info_.resources_directory);

    // Brief 04e M1: stand up the per-lane command/timeline infrastructure over the device's
    // capability-derived submission lanes. The main lane's timeline IS the frame-pacing
    // semaphore (one central timeline per queue; no ad-hoc semaphores for queue work).
    submissions_.init(device_.get_device(), device_.submission_lanes(), frames_in_flight_);
    main_lane_ = submissions_.lane_index("main");
    frame_semaphore_ = submissions_.timeline(main_lane_);
    // Brief 04e M4: async placement targets the first async compute lane when it exists.
    async_lane_ = submissions_.lane_index("async-compute-0");

    // Frame-capture config via CVars. Touch the accessors so both are registered, apply the env
    // bridge (honours STRING_R_CAPTURE_FRAME and legacy STRING_CAPTURE_FRAME/PATH aliases), then
    // read. apply_env() is idempotent and cheap; calling it here initialises these levers.
    capture_frame_cvar();
    capture_path_cvar();
    // Touch the capture-sequence CVar too BEFORE apply_env, or its env override (STRING_CAPTURE_EVERY_N
    // / STRING_R_CAPTURE_EVERY_N) is never registered and headless capture sequences are silently
    // ignored (the accessor is otherwise first touched at frame end, long after apply_env ran).
    capture_every_n_cvar();
    async_enabled_cvar();   // register before apply_env so STRING_ASYNC works headlessly
    string::core::CVarRegistry::instance().apply_env();
    capture_frame_ = static_cast<uint64_t>(std::max(0, capture_frame_cvar().get()));
    capture_path_ = capture_path_cvar().get();

    window_->register_resize_event_callback(std::bind(&Renderer::handle_resize, this, std::placeholders::_1));

    VkExtent2D extent = presenter_.get_extent();

    // Render targets. color_attachment_ is 1-sample: it's the MSAA resolve destination and the
    // texture the composite pass samples. msaa_color_ / msaa_depth_ are the multisampled targets
    // the scene passes actually render into.
    color_attachment_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    msaa_color_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .samples = msaa_samples_,
    });
    msaa_depth_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_D32_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Brief 04d: SAMPLED so the two-phase min-resolve compute can texelFetch its MSAA samples
        // to build the HiZ pyramid mip-0 (min = farthest, reverse-Z conservative) between phase-1
        // and phase-2 opaque draws. Still 4x MSAA D32; only read, never storage-written.
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .samples = msaa_samples_,
    });

    // Make the offscreen HDR target samplable by the composite pass via the bindless table.
    bind_composite_source();

    // Upload on the graphics queue: the texture's TRANSFER_DST -> SHADER_READ_ONLY barrier
    // uses a FRAGMENT_SHADER dst stage (only valid on a graphics-capable queue), and keeping
    // upload + sampling on one queue family avoids a queue-ownership transfer. (A dedicated
    // async-transfer queue would need explicit ownership transfers instead.)
    //
    // Build the application's declared passes now that the GPU context is ready. The plan
    // authors the content (which passes, in what order); the renderer just supplies the
    // context and executes. Passes that upload (e.g. geometry) record into the transfer batch
    // and bind into the bindless table during construction. The batch streams uploads
    // asynchronously (a ring of command buffers on a timeline), so building passes doesn't
    // stall per upload; wait_idle() below drains it once before the first frame. transfer_batch_
    // is a persistent member (it keeps streaming after init), flushed per frame in begin_frame.
    PassContext pass_context{
        device_,
        allocator_,
        global_descriptor_table_,
        shader_registry_,
        transfer_batch_,
        window_->get_input(),
        input_map_,
        string::gpu::COLOR_TARGET,
        string::gpu::DEPTH_TARGET,
        resources_path,
        static_cast<uint16_t>(frames_in_flight_),
        msaa_samples_,
        // Address of the Tracy GPU ctx member (created later in init_gpu_profiler); passes store the
        // pointer and read it live at record time. Brief 06.
        &gpu_profiler_ctx_,
        frame_scratch_,
    };
    scene_passes_ = plan.build(pass_context);

    // Brief 04e M3: all passes have declared their per-frame scratch needs; back them with one
    // device-local arena per frame slot (logged as the VRAM consolidation number).
    frame_scratch_.materialize(allocator_, frames_in_flight_);

    // Composite resolves the offscreen HDR target to the swapchain: reads color_attachment_,
    // writes the screen. Declared here (composite_pass_ is renderer-built, not in the plan).
    composite_pass_.usages = {
        { string::gpu::COLOR_TARGET,     Access::SampledRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT },
        { string::gpu::SWAPCHAIN_TARGET, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
    };

    // The frame graph's execution list: scene passes, then the composite resolve. written_
    // resources_ = everything the graph writes, so record_frame only transitions those (never
    // the static uploaded textures/buffers the transfer batch already left in SHADER_READ).
    for (auto& pass : scene_passes_)
        frame_passes_.push_back(pass.get());
    frame_passes_.push_back(&composite_pass_);
    // written_resources_ is recomputed per frame in record_frame (brief 04e M2): usages may name
    // per-frame-slot resources, so the written set is frame state, not init state.

    // Drain the async upload ring: submit any pending batch and wait for every in-flight batch
    // to finish (freeing all staging) before the first frame draws the uploaded resources.
    transfer_batch_.wait_idle();

    for (auto& pass : scene_passes_)
    {
        pass->resize(extent);
    }

    for (auto& frame : frames_)
    {
        frame.frame_id = 0;
    }

    init_gpu_profiler();
    gpu_timing_.init(device_, frames_in_flight_);
    GpuProfiler::set_global(&gpu_timing_);
}

// Stand up the Tracy GPU context on the graphics queue. Compiles to a no-op (ctx == nullptr) when
// -Dtracy is off. Uses frame 0's command buffer to probe the timestamp period; the buffer is reset
// again in the first begin_frame(), so this doesn't disturb normal recording.
//
// TracyVkContext* records+submits+waits on the probe buffer ITSELF (it calls vkBeginCommandBuffer
// internally). Our command pool is created without VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
// (buffers are recycled via vkResetCommandPool), so we must hand Tracy a buffer in the INITIAL
// state — never begin/end it ourselves here, or Tracy's begin becomes an illegal implicit reset.
void Renderer::init_gpu_profiler()
{
#if defined(STRING_PROFILE) && !defined(STRING_RELEASE)
    // Brief 04e M1: one Tracy GPU context PER LANE, named after it, so multi-queue overlap is
    // visible in traces. Each context probes with a command buffer from ITS lane's pool (Tracy
    // records/submits/waits on the probe itself — hand it an INITIAL-state buffer). Lanes whose
    // family has no valid timestamps get no context (Tracy timestamps would be meaningless).
    lane_profiler_ctxs_.assign(submissions_.lane_count(), nullptr);
    for (uint32_t i = 0; i < submissions_.lane_count(); ++i)
    {
        const string::gpu::submission_lane& lane = submissions_.lane(i);
        if (!lane.timestamps) continue;
        VkCommandBuffer probe = submissions_.recorder(i, 0).get_command_buffer();
        if (device_.supports_calibrated_timestamps())
        {
            STRING_PROFILE_GPU_CONTEXT_CREATE(lane_profiler_ctxs_[i], device_.get_physical_device(),
                device_.get_device(), lane.vk_queue, probe);
        }
        else
        {
            STRING_PROFILE_GPU_CONTEXT_CREATE_BASIC(lane_profiler_ctxs_[i],
                device_.get_physical_device(), device_.get_device(), lane.vk_queue, probe);
        }
        STRING_PROFILE_GPU_CONTEXT_NAME(lane_profiler_ctxs_[i], lane.name.c_str(),
            lane.name.size());
        submissions_.recorder(i, 0).reset();
    }
    gpu_profiler_ctx_ = lane_profiler_ctxs_[main_lane_];
#endif
}

Renderer::~Renderer()
{
    STRING_LOG_DEBUG("Waiting for device to be idle...");
    vkDeviceWaitIdle(device_.get_device());

    // Tear down the per-lane Tracy GPU contexts (no-op when profiling is off) before the device
    // goes away. gpu_profiler_ctx_ aliases the main lane's context — don't double-destroy it.
    for (STRING_PROFILE_GPU_CONTEXT_TYPE& ctx : lane_profiler_ctxs_)
        STRING_PROFILE_GPU_CONTEXT_DESTROY(ctx)
    lane_profiler_ctxs_.clear();
    gpu_profiler_ctx_ = nullptr;
    GpuProfiler::set_global(nullptr);
    gpu_timing_.destroy(device_.get_device());

    // Destroy the passes while the allocator, table, and device are still alive.
    scene_passes_.clear();

    for (auto& frame : frames_)
    {
        frame.garbage_collector.flush();
    }
    // Destroys every lane's recorders + timelines (frame_semaphore_ aliases the main timeline).
    submissions_.destroy();

    allocator_.destroy_resource(msaa_depth_);
    allocator_.destroy_resource(msaa_color_);
    allocator_.destroy_resource(color_attachment_);
    frame_scratch_.destroy(allocator_);
}

void Renderer::update()
{
    STRING_PROFILE_SCOPE("update (CPU passes)")
    // Real per-frame delta (seconds since the previous update), for framerate-independent
    // motion like the camera. First frame clamps to ~0.
    static auto last_time = std::chrono::high_resolution_clock::now();
    const auto current_time = std::chrono::high_resolution_clock::now();
    float delta_time =
        std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_time).count();
    last_time = current_time;
    // STRING_FIXED_DT=<seconds> forces a deterministic per-frame timestep, so scripted motion
    // (dbg.orbit) advances by a fixed amount every frame regardless of wall-clock jitter. Essential
    // for headless A/B under MOTION: HiZ-on vs HiZ-off captures at the same frame index then land at
    // the SAME camera pose (wall-clock dt otherwise desynchronizes them). 0/unset = real dt.
    static const float fixed_dt = [] {
        if (const char* e = std::getenv("STRING_FIXED_DT")) { float v = std::atof(e); if (v > 0.0f) return v; }
        return 0.0f;
    }();
    if (fixed_dt > 0.0f) delta_time = fixed_dt;

    // Frame time on the Tracy timeline (ms). Cheap literal-named plot; no-op without -Dtracy.
    STRING_PROFILE_PLOT("frame time (ms)", delta_time * 1000.0f)

    // Brief 06: reset the immediate-mode debug-draw ring at the top of the frame. Systems then
    // accumulate lines/labels during update()/record(); the debug line pass drains it in record().
    string::debug::context().clear();

    // Rolling frame-time log (until Tracy is wired): avg/max ms per 600 frames.
    static float acc = 0.0f, worst = 0.0f;
    static uint32_t n = 0;
    acc += delta_time; worst = std::max(worst, delta_time); ++n;
    if (n == 600)
    {
        STRING_LOG_INFO("[frametime] avg {:.2f} ms ({:.0f} fps), worst {:.2f} ms",
                        acc / n * 1000.0f, n / acc, worst * 1000.0f);
        // Brief 06: append the per-pass GPU breakdown (in-game timestamp readback) to the same
        // cadence, so headless logs carry the GPU cost split, not just the CPU frame total.
        if (gpu_timing_.enabled())
        {
            const std::vector<GpuProfiler::PassStat> ps = gpu_timing_.stats();
            std::string line = "[frametime] gpu per-pass (ms):";
            for (const GpuProfiler::PassStat& s : ps)
                line += std::format(" {}={:.3f}", s.name, s.avg_ms);
            line += std::format(" | total={:.3f}", gpu_timing_.total_avg_ms());
            STRING_LOG_INFO("{}", line);
        }
        acc = 0.0f; worst = 0.0f; n = 0;
    }

    for (auto& pass : scene_passes_)
    {
        STRING_PROFILE_SCOPE_DYNAMIC(pass->debug_name().data(), pass->debug_name().size())
        pass->update(delta_time, static_cast<uint16_t>(current_frame_));
    }
}

void Renderer::begin_frame()
{
    STRING_PROFILE_SCOPE("begin_frame")
    auto& frame = frames_[current_frame_];

    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &frame_semaphore_,
        .pValues = &frame.frame_id,
    };
    vkWaitSemaphores(device_.get_device(), &wait_info, UINT64_MAX);

    frame.garbage_collector.flush();
    main_recorder(current_frame_).reset();

    // Shader hot-reload, at the frame boundary (GPU work for this slot has completed — see the
    // semaphore wait above). Poll the file watcher for edits, then apply any completed recompiles:
    // swap the rebuilt pipeline in and retire the old one through THIS frame's garbage collector,
    // so it is destroyed only after the ring cycles back (past all in-flight frames).
    {
        STRING_PROFILE_SCOPE("shader hot-reload poll")
        file_watcher_.poll_main_thread();
    }
    shader_registry_.apply_pending_swaps([&frame, this](string::gpu::pipeline old) {
        frame.garbage_collector.push_function([this, old] {
            vkDestroyPipeline(device_.get_device(), old.pipeline, nullptr);
            vkDestroyPipelineLayout(device_.get_device(), old.pipeline_layout, nullptr);
        });
    });

    update();

    // Push any streamed uploads recorded during update() onto the graphics queue (submit, don't
    // wait). A no-op until a streamer records into the batch; the trailing transfer barrier makes
    // the writes visible to this frame's draws via submission order on the shared queue.
    {
        STRING_PROFILE_SCOPE("transfer_batch flush")
        transfer_batch_.flush();
    }

    // Acquire now, before recording, so end_rendering has a valid blit target. Copy the
    // handles out of string::gpu::acquired_image (which holds references into vectors resize() reallocates).
    string::gpu::acquired_image acquired = presenter_.acquire_next_frame();
    acquired_image_ = acquired.image;
    acquired_image_view_ = acquired.image_view;
    acquired_wait_semaphore_ = acquired.wait_for_image_available;
    acquired_signal_semaphore_ = acquired.signal_when_ready_to_present;
}

void Renderer::record_frame()
{
    STRING_PROFILE_SCOPE("record_frame")
    string::gpu::command_recorder& recorder = main_recorder(current_frame_);
    VkCommandBuffer command_buffer = recorder.begin();
    const VkExtent2D extent = presenter_.get_extent();

    // Brief 06: reset this frame index's GPU-timestamp pool (and read back its previous cycle's
    // results — that GPU work is provably complete since the renderer already waited on this frame
    // index's timeline value). Must precede any timestamp writes below.
    gpu_timing_.begin_frame(command_buffer, static_cast<uint32_t>(current_frame_));

    const VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    const VkRect2D scissor = { .offset = { 0, 0 }, .extent = extent };

    // Brief 04e M2: the frame graph. Every frame the passes' declared ResourceUsages feed the
    // planner; execution follows its (stable, authored-order-preserving) toposort, and ALL
    // inter-pass barriers below derive from the same declarations through resource_states_ —
    // pass-declared usage is the single source of truth for scheduling AND sync.
    GraphBuilder graph_builder;
    for (Pass* pass : frame_passes_)
    {
        PassBuilder pass_builder = graph_builder.add_pass(std::string(pass->debug_name()));
        for (const ResourceUsage& usage : pass->usages)
            pass_builder.use(usage.resource, usage.access, usage.stage);
        pass_builder.end_pass();
    }
    const RenderGraph graph = graph_builder.build();
    // Brief 04e M3: one-shot lifetime report — the planner's per-resource [first, last] topo
    // positions are the transient-aliasing input. Two transients may share memory iff their
    // spans are disjoint (plus the aliasing rules: acquire-from-UNDEFINED vs the previous
    // tenant's scope, and never across frames-in-flight slots).
    static bool logged_lifetimes = false;
    if (!logged_lifetimes)
    {
        logged_lifetimes = true;
        for (const auto& [id, lifetime] : graph.resource_lifetimes)
            STRING_LOG_INFO("[graph] resource {}: passes [{}..{}] first_writer={}",
                            id, lifetime.first.value_or(0), lifetime.last.value_or(0),
                            lifetime.first_writer.has_value()
                                ? static_cast<int64_t>(*lifetime.first_writer) : -1);
    }
    std::vector<Pass*> execution_order;
    execution_order.reserve(frame_passes_.size());
    for (uint32_t index : graph.toposorted)
        execution_order.push_back(frame_passes_[index]);

    // The set of resources the graph itself writes this frame (usages may change per frame —
    // per-frame-slot buffers). Static uploaded inputs are never re-transitioned.
    written_resources_.clear();
    for (const Pass* pass : frame_passes_)
        for (const ResourceUsage& usage : pass->usages)
            if (is_write(usage.access))
                written_resources_.insert(usage.resource);

    // A usage recorded during the compute prepass (record_compute) vs during the graphics groups
    // (record). The stage mask says which side of the frame it belongs to.
    const auto is_compute_stage = [](VkPipelineStageFlags2 stage) {
        return (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0;
    };
    // Buffer usages carry no image layout; they go through the tracker's merged memory barrier.
    const auto is_buffer_usage = [](const ResourceUsage& usage) {
        return access_scope(usage.access).layout == VK_IMAGE_LAYOUT_UNDEFINED;
    };

    // Brief 04e M4: place the frame's dependency-free async-compute chains. With an async lane
    // (and r.async.enabled): record ALL chains into ONE command buffer on that lane, submit it
    // now signalling the lane timeline at frame_count_, register the cross-lane wait for the
    // main submit (at the union of the declared read stages), and queue-family-transfer the
    // written buffers — release recorded on the async queue, acquire recorded here on main
    // (explicit transfers by default, per the locked decision). Without a lane the SAME chains
    // record inline on the main queue with tracker-derived barriers — zero special cases.
    // async_usages semantics: write usages = produced by the chain ON the async queue; read
    // usages = consumed by the main-queue frame. Async-queue INPUTS that are host-written (the
    // light ring) need no declaration — their contents come from the host domain, which is not
    // queue-family scoped.
    async_waits_.clear();
    {
        std::vector<Pass*> async_passes;
        for (Pass* pass : execution_order)
            if (pass->has_async_compute()) async_passes.push_back(pass);
        const bool place_async = !async_passes.empty() && async_lane_ != UINT32_MAX
                              && async_enabled_cvar().get();
        if (place_async)
        {
            const string::gpu::submission_lane& lane = submissions_.lane(async_lane_);
            STRING_PROFILE_GPU_CONTEXT_TYPE async_ctx =
                async_lane_ < lane_profiler_ctxs_.size() ? lane_profiler_ctxs_[async_lane_] : nullptr;
            string::gpu::command_recorder& async_recorder =
                submissions_.recorder(async_lane_, static_cast<uint32_t>(current_frame_));
            async_recorder.reset();
            VkCommandBuffer async_cb = async_recorder.begin();
            std::vector<VkBufferMemoryBarrier2> releases;
            std::vector<VkBufferMemoryBarrier2> acquires;
            VkPipelineStageFlags2 wait_stages = 0;
            for (Pass* pass : async_passes)
            {
                {
                    STRING_PROFILE_GPU_ZONE_DYNAMIC(async_ctx, async_cb, "froxel-cull")
                    pass->record_async_compute(async_recorder, static_cast<uint16_t>(current_frame_));
                }
                for (const ResourceUsage& usage : pass->async_usages)
                {
                    const AccessScope scope = access_scope(usage.access);
                    const VkBuffer buffer = allocator_.get_buffer(usage.resource).buffer;
                    if (is_write(usage.access))
                    {
                        // Release half of the QFOT (the written contents must survive the
                        // family transfer for the main-queue reads).
                        releases.push_back(VkBufferMemoryBarrier2{
                            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                            .pNext = nullptr,
                            .srcStageMask = usage.stage,
                            .srcAccessMask = scope.access,
                            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
                            .dstAccessMask = 0,
                            .srcQueueFamilyIndex = lane.queue_family_index,
                            .dstQueueFamilyIndex = graphics_queue_.queue_family_index,
                            .buffer = buffer,
                            .offset = 0,
                            .size = VK_WHOLE_SIZE,
                        });
                    }
                    else
                    {
                        wait_stages |= usage.stage;
                        acquires.push_back(VkBufferMemoryBarrier2{
                            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                            .pNext = nullptr,
                            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                            .srcAccessMask = 0,
                            .dstStageMask = usage.stage,
                            .dstAccessMask = scope.access,
                            .srcQueueFamilyIndex = lane.queue_family_index,
                            .dstQueueFamilyIndex = graphics_queue_.queue_family_index,
                            .buffer = buffer,
                            .offset = 0,
                            .size = VK_WHOLE_SIZE,
                        });
                    }
                }
            }
            if (!releases.empty())
            {
                const VkDependencyInfo release_dep = {
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .pNext = nullptr,
                    .dependencyFlags = 0,
                    .memoryBarrierCount = 0,
                    .pMemoryBarriers = nullptr,
                    .bufferMemoryBarrierCount = static_cast<uint32_t>(releases.size()),
                    .pBufferMemoryBarriers = releases.data(),
                    .imageMemoryBarrierCount = 0,
                    .pImageMemoryBarriers = nullptr,
                };
                vkCmdPipelineBarrier2(async_cb, &release_dep);
            }
            STRING_PROFILE_GPU_COLLECT(async_ctx, async_cb)
            async_recorder.end();

            const VkSemaphoreSubmitInfo async_signal = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = submissions_.timeline(async_lane_),
                .value = frame_count_,
                .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .deviceIndex = 0,
            };
            const VkCommandBufferSubmitInfo async_cb_info = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = async_cb,
                .deviceMask = 0,
            };
            const VkSubmitInfo2 async_submit = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .pNext = nullptr,
                .flags = 0,
                .waitSemaphoreInfoCount = 0,
                .pWaitSemaphoreInfos = nullptr,
                .commandBufferInfoCount = 1,
                .pCommandBufferInfos = &async_cb_info,
                .signalSemaphoreInfoCount = 1,
                .pSignalSemaphoreInfos = &async_signal,
            };
            if (vkQueueSubmit2(lane.vk_queue, 1, &async_submit, nullptr) != VK_SUCCESS)
                throw std::runtime_error("failed to submit async compute command buffer");
            submissions_.mark_signaled(async_lane_, frame_count_);

            // Acquire half of the QFOT on main + the cross-lane timeline wait for end_frame.
            if (!acquires.empty())
            {
                const VkDependencyInfo acquire_dep = {
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .pNext = nullptr,
                    .dependencyFlags = 0,
                    .memoryBarrierCount = 0,
                    .pMemoryBarriers = nullptr,
                    .bufferMemoryBarrierCount = static_cast<uint32_t>(acquires.size()),
                    .pBufferMemoryBarriers = acquires.data(),
                    .imageMemoryBarrierCount = 0,
                    .pImageMemoryBarriers = nullptr,
                };
                vkCmdPipelineBarrier2(command_buffer, &acquire_dep);
            }
            async_waits_.push_back(VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = submissions_.timeline(async_lane_),
                .value = frame_count_,
                .stageMask = wait_stages != 0 ? wait_stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            });
        }
        else
        {
            // Inline degrade path: same chains, main queue, tracker-derived barriers.
            for (Pass* pass : async_passes)
            {
                for (const ResourceUsage& usage : pass->async_usages)
                    if (is_write(usage.access))
                        resource_states_.buffer_access(usage.resource, usage.access, usage.stage);
                resource_states_.flush_buffers(command_buffer);
                {
                    STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, "froxel-cull")
                    gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_),
                                            std::string(pass->debug_name()) + " (async-inline)");
                    pass->record_async_compute(recorder, static_cast<uint16_t>(current_frame_));
                    gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
                }
                // The main-queue reads: noted now, flushed with the next barrier point (before
                // any graphics group begins).
                for (const ResourceUsage& usage : pass->async_usages)
                    if (!is_write(usage.access))
                        resource_states_.buffer_access(usage.resource, usage.access, usage.stage);
            }
        }
    }

    // Compute prepass: passes may dispatch GPU work (e.g. GPU culling that fills indirect
    // worklists) outside dynamic rendering, before any graphics group. Each pass's declared
    // compute-stage buffer usages derive the barriers it needs (vs prior tracked accesses).
    for (Pass* pass : execution_order)
    {
        for (const ResourceUsage& usage : pass->usages)
            if (is_buffer_usage(usage) && is_compute_stage(usage.stage))
                resource_states_.buffer_access(usage.resource, usage.access, usage.stage);
        resource_states_.flush_buffers(command_buffer);

        const std::string pass_name(pass->debug_name());
        STRING_PROFILE_SCOPE_DYNAMIC(pass_name.data(), pass_name.size())
        STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, pass_name.c_str())
        gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_), pass_name + " (cs)");
        pass->record_compute(recorder, static_cast<uint16_t>(current_frame_));
        gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
    }

    // The color / depth target a pass renders into (every frame pass writes exactly one color
    // target; string::gpu::SWAPCHAIN_TARGET means the screen).
    const auto color_target_of = [](const Pass* pass) -> string::gpu::resource_id {
        for (const ResourceUsage& usage : pass->usages)
            if (usage.access == Access::ColorWrite) return usage.resource;
        return string::gpu::SWAPCHAIN_TARGET;
    };
    const auto depth_target_of = [](const Pass* pass) -> std::optional<string::gpu::resource_id> {
        for (const ResourceUsage& usage : pass->usages)
            if (usage.access == Access::DepthWrite) return usage.resource;
        // Depth READS must also bind the depth image as an attachment: depth-tested/write-off
        // pipelines (debug lines) need it, and — critically — the reopened group that hosts a
        // breaker's phase-2 draws inherits THIS group's depth attachment. Matching only DepthWrite
        // left that group with pDepthAttachment = NULL, so the phase-2 opaque + sorted-transparency
        // draws ran with NO depth test at all: every disoccluded meshlet landed on top of the scene
        // (far geometry over near = persistent semi-transparent surfaces, popping under motion,
        // clean with HiZ off because single-pass never splits the group).
        for (const ResourceUsage& usage : pass->usages)
            if (usage.access == Access::DepthRead) return usage.resource;
        return std::nullopt;
    };

    // Group consecutive passes that share a color target and run each group as one render-pass
    // instance. All barriers are derived from the passes' declared usages via resource_states_.
    //
    // Brief 04d: a pass may declare breaks_scene_group() to force the group to END after it so a
    // COMPUTE step (record_between — the HiZ min-resolve + pyramid) can run OUTSIDE rendering before
    // the next group. Consecutive COLOR_TARGET (MSAA) groups then form a chain: the FIRST clears the
    // MSAA color+depth, later ones LOAD (preserve) them, and only the LAST resolves msaa_color_ ->
    // color_attachment_ (intermediate MSAA groups STORE their samples for the next to load).
    size_t start = 0;
    bool seen_msaa_group = false;   // has a COLOR_TARGET group already cleared the MSAA targets?
    Pass* pending_after_between = nullptr;   // breaker whose phase-2 runs in the next (reopened) group
    while (start < execution_order.size())
    {
        const string::gpu::resource_id group_color = color_target_of(execution_order[start]);
        std::optional<string::gpu::resource_id> group_depth;
        size_t end = start;
        bool break_after = false;
        while (end < execution_order.size() && color_target_of(execution_order[end]) == group_color)
        {
            if (auto depth = depth_target_of(execution_order[end])) group_depth = depth;
            const bool breaks = execution_order[end]->breaks_scene_group();
            ++end;
            if (breaks) { break_after = true; break; }   // end this group right after the breaker
        }

        // Is this the LAST COLOR_TARGET (MSAA) group in the chain? Only the last one resolves; the
        // rest STORE their MSAA samples for the following group to LOAD. A group is the last MSAA
        // group iff it writes COLOR_TARGET and the pass immediately after `end` does NOT.
        const bool this_is_msaa = (group_color == string::gpu::COLOR_TARGET);
        const bool next_is_msaa = this_is_msaa && end < execution_order.size()
            && color_target_of(execution_order[end]) == string::gpu::COLOR_TARGET;
        const bool msaa_is_first = this_is_msaa && !seen_msaa_group;   // clears vs loads
        const bool msaa_is_last = this_is_msaa && !next_is_msaa;       // resolves vs stores
        if (this_is_msaa) seen_msaa_group = true;

        // Brief 04d: a breaking group MIN-resolves its MSAA depth into the breaker's single-sample
        // depth-resolve target (reverse-Z: min = farthest = conservative HiZ occluder), so
        // record_between() can build the pyramid from a normally-samplable depth. 0 = no resolve.
        const string::gpu::resource_id depth_resolve = break_after
            ? execution_order[end - 1]->depth_resolve_target(static_cast<uint16_t>(current_frame_)) : 0;

        // Barriers: transition each graph-written image the group touches to the state its usage
        // needs (deduped per resource). Static uploaded inputs are skipped — they aren't in
        // written_resources_, and buffer usages map to VK_IMAGE_LAYOUT_UNDEFINED. Reads of a
        // graph-written resource (the composite sampling color) get the correct source layout
        // from the tracker; writes discard the previous contents.
        std::unordered_map<string::gpu::resource_id, ResourceUsage> group_transitions;
        for (size_t i = start; i < end; ++i)
            for (const ResourceUsage& usage : execution_order[i]->usages)
            {
                // Graphics-phase BUFFER usages (indirect worklists, task/fragment storage reads,
                // the visibility bitfield's task-stage RMW): note them with the tracker so their
                // barriers — vs this frame's compute writes AND the previous frame's draws —
                // derive from the declaration. Flushed as one merged barrier before rendering.
                if (is_buffer_usage(usage))
                {
                    if (!is_compute_stage(usage.stage))
                        resource_states_.buffer_access(usage.resource, usage.access, usage.stage);
                    continue;
                }
                if (!written_resources_.contains(usage.resource)) continue;
                // Writes take precedence: a group that both writes and reads a resource as an
                // attachment must sit in the WRITE layout for the whole rendering (the group's
                // attachment info uses ATTACHMENT_OPTIMAL; attachment reads are legal there). A
                // read-usage last-wins here once put depth in READ_ONLY under a group whose
                // rendering declared ATTACHMENT_OPTIMAL -> per-frame validation errors.
                auto [it, inserted] = group_transitions.try_emplace(usage.resource, usage);
                if (!inserted && is_write(usage.access) && !is_write(it->second.access))
                    it->second = usage;
            }
        // The group's depth image is bound as a read-write attachment (depth_attachment_info uses
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL) whenever group_depth is set — even if the group's own
        // passes only READ depth (debug lines). Force the write scope so the tracked layout matches
        // the attachment layout, and so it covers the breaker's phase-2 depth WRITES, which are
        // recorded into this group ahead of its own passes. Without this, a read-only group would
        // sit in the READ_ONLY layout while rendering declares ATTACHMENT_OPTIMAL.
        if (group_depth && written_resources_.contains(*group_depth))
        {
            const ResourceUsage depth_rw = { *group_depth, Access::DepthWrite,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT };
            auto [it, inserted] = group_transitions.try_emplace(*group_depth, depth_rw);
            if (!inserted && !is_write(it->second.access)) it->second = depth_rw;
        }
        for (const auto& [resource, usage] : group_transitions)
        {
            const VkImageAspectFlags aspect =
                (usage.access == Access::DepthWrite || usage.access == Access::DepthRead)
                    ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            // Brief 04d: a non-first MSAA group LOADs the depth phase-1 wrote — do NOT discard it
            // (discard would drop the phase-1 depth the pyramid/phase-2 depend on). record_between()
            // already put msaa_depth_ back in DEPTH_ATTACHMENT after the min-resolve read.
            const bool load_depth = this_is_msaa && !msaa_is_first
                && aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
            resource_states_.transition(command_buffer, image_of(resource), aspect,
                usage.access, usage.stage, /*discard=*/is_write(usage.access) && !load_depth);
        }

        // The scene group (COLOR_TARGET) is multisampled: passes render into msaa_color_ and it's
        // resolved into color_attachment_ (which the group_transitions loop already moved to
        // COLOR_ATTACHMENT_OPTIMAL as the resolve dest). The composite group (SWAPCHAIN) is
        // single-sample and renders straight into the swapchain image.
        //
        // Brief 04e M2: msaa_color_ is a FIRST-CLASS tracked resource now. The logical
        // COLOR_TARGET the passes declare expands here to the physical MSAA image; the
        // hazard-complete tracker derives every barrier it needs:
        //   - first MSAA group (discard): synchronizes the clear+writes against the PREVIOUS
        //     frame's still-in-flight resolve read/writes (the 04d cross-frame semi-transparency
        //     race — ColorWrite's scope includes the resolve READ);
        //   - later MSAA groups (no discard): a WAW/store->load self-barrier orders this group's
        //     load + writes + resolve against the previous group's store (the 04d motion-ghosting
        //     bug). Both former hand-rolled vkCmdPipelineBarrier2 blocks are DELETED — this is
        //     exactly the class of sync that must derive from declarations, not accrete by hand.
        const bool msaa_group = this_is_msaa;
        if (msaa_group)
        {
            resource_states_.transition(command_buffer, allocator_.get_image(msaa_color_).image,
                VK_IMAGE_ASPECT_COLOR_BIT, Access::ColorWrite,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, /*discard=*/msaa_is_first);
        }
        // Emit the merged buffer barrier for this group's declared buffer usages (outside
        // rendering — task/indirect/fragment reads of the compute-built worklists, the visibility
        // bitfield's cross-frame task RMW).
        resource_states_.flush_buffers(command_buffer);

        // Brief 04d MSAA chain: first group clears, later groups load; only the last resolves.
        const VkAttachmentLoadOp msaa_color_load =
            msaa_is_first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        const VkAttachmentStoreOp msaa_color_store =
            msaa_is_last ? VK_ATTACHMENT_STORE_OP_DONT_CARE   // resolved, MS samples not kept
                         : VK_ATTACHMENT_STORE_OP_STORE;      // kept for the next MSAA group to load
        const VkRenderingAttachmentInfo color_attachment_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = msaa_group ? allocator_.get_image(msaa_color_).view : image_view_of(group_color),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            // Resolve the multisampled scene color into color_attachment_ only on the LAST MSAA group.
            .resolveMode = (msaa_group && msaa_is_last) ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
            .resolveImageView = (msaa_group && msaa_is_last) ? image_view_of(group_color) : VK_NULL_HANDLE,
            .resolveImageLayout = (msaa_group && msaa_is_last) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = msaa_group ? msaa_color_load : VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = msaa_group ? msaa_color_store : VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = {{ 0.0f, 0.0f, 0.0f, 0.0f }} },
        };
        // Depth: first MSAA group clears, later ones load the phase-1 depth. STORE it while more
        // MSAA groups follow (phase-2 + transparency depth-test against it); the last may drop it.
        const VkAttachmentLoadOp depth_load =
            (msaa_group && !msaa_is_first) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        const VkAttachmentStoreOp depth_store =
            (msaa_group && !msaa_is_last) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Brief 04d: MIN depth resolve into the breaker's single-sample target (if any). Transition it
        // to the depth-attachment (resolve dest) layout first; record_between() moves it to SHADER_READ.
        // Vulkan resolve ops write in COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE (even a depth
        // resolve), so this layout transition must be made available to that stage/access — else
        // sync-validation flags a WRITE_AFTER_WRITE between the transition and the EndRendering resolve.
        // Direct barrier (not the tracker's DepthWrite scope, which would name LATE_FRAGMENT_TESTS):
        // record_between() manages hz.depth's subsequent transitions with its own hardcoded barriers,
        // and it is re-discarded (UNDEFINED) here every frame, so the tracker need not track it.
        if (depth_resolve != 0)
            vku::transition_image(command_buffer, {
                .image = image_of(depth_resolve),
                .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,   // discard: prior contents not needed
                .new_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,   // last used by the pyramid reduce
                .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .dst_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,  // the resolve write stage
                .dst_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
            });
        const VkRenderingAttachmentInfo depth_attachment_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = group_depth ? image_view_of(*group_depth) : VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .resolveMode = depth_resolve != 0 ? VK_RESOLVE_MODE_MIN_BIT : VK_RESOLVE_MODE_NONE,
            .resolveImageView = depth_resolve != 0 ? image_view_of(depth_resolve) : VK_NULL_HANDLE,
            .resolveImageLayout = depth_resolve != 0 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = depth_load,
            .storeOp = depth_store,
            // Reverse-Z: far plane is 0 (see the depth pipeline's GREATER_OR_EQUAL compare).
            .clearValue = { .depthStencil = { 0.0f, 0 } },
        };
        const VkRenderingInfo rendering_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderArea = {{ 0, 0 }, extent},
            .layerCount = 1,
            .viewMask = 0,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_attachment_info,
            .pDepthAttachment = group_depth ? &depth_attachment_info : nullptr,
            .pStencilAttachment = nullptr,
        };

        vkCmdBeginRendering(command_buffer, &rendering_info);
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &scissor);

        // Brief 04d: the previous group's breaker draws its phase-2 geometry here, into the reloaded
        // MSAA targets, tested against the pyramid its record_between() just built — before this
        // group's own passes.
        if (pending_after_between)
        {
            const std::string an(pending_after_between->debug_name());
            STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, "phase2")
            gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_), an + " (phase2)");
            pending_after_between->record_after_between(recorder, static_cast<uint16_t>(current_frame_));
            gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
            pending_after_between = nullptr;
        }

        for (size_t i = start; i < end; ++i)
        {
            const std::string pass_name(execution_order[i]->debug_name());
            STRING_PROFILE_SCOPE_DYNAMIC(pass_name.data(), pass_name.size())
            STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, pass_name.c_str())
            gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_), pass_name);
            execution_order[i]->record(recorder, static_cast<uint16_t>(current_frame_));
            gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
        }

        vkCmdEndRendering(command_buffer);

        // Brief 04d: the breaker's compute step (HiZ min-resolve + pyramid) runs here — OUTSIDE
        // rendering, after phase-1's depth is stored, before phase-2's group reopens.
        if (break_after)
        {
            Pass* breaker = execution_order[end - 1];
            const std::string bn(breaker->debug_name());
            STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, "hiz-build")
            gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_), bn + " (between)");
            breaker->record_between(recorder, static_cast<uint16_t>(current_frame_));
            gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
            pending_after_between = breaker;   // its phase-2 runs in the next group
        }
        start = end;
    }

    // The swapchain was rendered by the final group; ready it for presentation.
    resource_states_.transition(command_buffer, acquired_image_, VK_IMAGE_ASPECT_COLOR_BIT,
        Access::Present, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

    // Collect this frame's GPU timestamp queries into the command buffer (once per frame, on the
    // still-recording buffer). Non-blocking — Tracy reads back completed queries opportunistically
    // across the frames-in-flight ring. No-op without -Dtracy.
    STRING_PROFILE_GPU_COLLECT(gpu_profiler_ctx_, command_buffer)

    recorder.end();
}

VkImage Renderer::image_of(string::gpu::resource_id target) const
{
    switch (target)
    {
        case string::gpu::SWAPCHAIN_TARGET: return acquired_image_;
        case string::gpu::COLOR_TARGET:     return allocator_.get_image(color_attachment_).image;
        case string::gpu::DEPTH_TARGET:     return allocator_.get_image(msaa_depth_).image;
        default:               return allocator_.get_image(target).image;
    }
}

VkImageView Renderer::image_view_of(string::gpu::resource_id target) const
{
    switch (target)
    {
        case string::gpu::SWAPCHAIN_TARGET: return acquired_image_view_;
        case string::gpu::COLOR_TARGET:     return allocator_.get_image(color_attachment_).view;
        case string::gpu::DEPTH_TARGET:     return allocator_.get_image(msaa_depth_).view;
        default:               return allocator_.get_image(target).view;
    }
}

void Renderer::end_frame()
{
    auto& frame = frames_[current_frame_];

    // The swapchain image was acquired in begin_frame; wait on image-availability at the
    // COLOR_ATTACHMENT_OUTPUT stage since the first thing we do to it is the composite draw.
    // Brief 04e M4: plus any cross-lane timeline waits the async placement registered this
    // frame (each at the union of its declared read stages).
    std::vector<VkSemaphoreSubmitInfo> wait_semaphore_infos = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = acquired_wait_semaphore_,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0
        }
    };
    wait_semaphore_infos.insert(wait_semaphore_infos.end(), async_waits_.begin(), async_waits_.end());

    const VkSemaphoreSubmitInfo signal_semaphore_infos[] = {
        {   // Render semaphore for present synchronization
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = acquired_signal_semaphore_,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0
        },
        {   // Timeline semaphore for each frame
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = frame_semaphore_,
            .value = frame_count_, // Don't signal the frame's id, signal the actual frame count
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0
        },
    };

    VkCommandBufferSubmitInfo command_buffer_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = main_recorder(current_frame_).get_command_buffer(),
        .deviceMask = 0
    };

    VkSubmitInfo2 submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = static_cast<uint32_t>(wait_semaphore_infos.size()),
        .pWaitSemaphoreInfos = wait_semaphore_infos.data(),
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_buffer_info,
        .signalSemaphoreInfoCount = 2,
        .pSignalSemaphoreInfos = signal_semaphore_infos
    };

    if (vkQueueSubmit2(graphics_queue_.queue, 1, &submit_info, nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    frame.frame_id = frame_count_;
    submissions_.mark_signaled(main_lane_, frame_count_);

    presenter_.present();

    if (capture_frame_ != 0 && frame_count_ >= capture_frame_)
    {
        capture_color_target(capture_path_);
        capture_frame_ = 0;
    }

    // Capture-sequence (r.capture.every_n): every Nth frame to a numbered file. Read live so the
    // console can start/stop it at runtime. Independent of the single-shot capture above.
    if (const int32_t every = capture_every_n_cvar().get();
        every > 0 && (frame_count_ % static_cast<uint64_t>(every)) == 0)
    {
        capture_color_target(numbered_capture_path(capture_path_cvar().get(), frame_count_));
    }

    // Increment frame
    frame_count_++;
    current_frame_ = frame_count_ % frames_in_flight_;
}

// Debug capture: drain the GPU, copy color_attachment_ (single-sample resolved HDR) to a host
// buffer, tonemap to 8-bit, write a bottom-up 24-bit BMP. Transitions go through
// resource_states_ so the tracker stays consistent for the next frame.
void Renderer::capture_color_target(const std::string& path)
{
    vkDeviceWaitIdle(device_.get_device());

    const VkExtent2D extent = presenter_.get_extent();
    const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 8;  // RGBA16F
    const string::gpu::resource_id staging = allocator_.create_resource(string::gpu::buffer_info{
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    });

    string::gpu::command_recorder recorder;
    recorder.init(device_.get_device(), graphics_queue_);
    VkCommandBuffer cb = recorder.begin();
    const string::gpu::allocated_image& src = allocator_.get_image(color_attachment_);
    resource_states_.transition(cb, src.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                Access::TransferRead, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    const VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { extent.width, extent.height, 1 },
    };
    vkCmdCopyImageToBuffer(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           allocator_.get_buffer(staging).buffer, 1, &region);
    recorder.end().immediate_submit();
    recorder.destroy();

    const auto half_to_float = [](uint16_t h) -> float {
        const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
        float v;
        if (exp == 0) v = man / 1024.0f / 16384.0f;
        else if (exp == 31) v = 65504.0f;
        else v = (1.0f + man / 1024.0f) * std::pow(2.0f, int(exp) - 15);
        return sign ? -v : v;
    };
    const auto encode = [&](uint16_t h) -> uint8_t {
        float v = std::max(half_to_float(h), 0.0f);
        v = v / (1.0f + v);                      // simple tonemap
        v = std::pow(v, 1.0f / 2.2f);            // gamma
        return uint8_t(std::min(v, 1.0f) * 255.0f + 0.5f);
    };

    const uint16_t* pixels =
        static_cast<const uint16_t*>(allocator_.get_buffer(staging).allocation_info.pMappedData);

    // Brief 06: PNG branch when the path ends ".png" (case-insensitive). Existing agent recipes use
    // .bmp and take the byte-identical legacy path below; only the ".png" suffix opts into PNG, so no
    // recipe regresses. PNG is top-down RGB8 (no BGR/row-flip surprises for diffing tools).
    const auto ends_with_png = [](const std::string& p) {
        if (p.size() < 4) return false;
        std::string s = p.substr(p.size() - 4);
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s == ".png";
    };

    if (ends_with_png(path))
    {
        std::vector<uint8_t> rgb(std::size_t(extent.width) * extent.height * 3);
        for (uint32_t y = 0; y < extent.height; ++y)
        {
            uint8_t* row = rgb.data() + std::size_t(y) * extent.width * 3;  // top-down
            const uint16_t* src_row = pixels + VkDeviceSize(y) * extent.width * 4;
            for (uint32_t x = 0; x < extent.width; ++x)
            {
                row[x * 3 + 0] = encode(src_row[x * 4 + 0]);  // R
                row[x * 3 + 1] = encode(src_row[x * 4 + 1]);  // G
                row[x * 3 + 2] = encode(src_row[x * 4 + 2]);  // B
            }
        }
        const std::vector<uint8_t> png =
            string::core::png::encode(rgb.data(), extent.width, extent.height, 3);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
        out.close();
        allocator_.destroy_resource(staging);
        STRING_LOG_INFO("[capture] frame {} -> {} (png)", frame_count_, path);
        return;
    }

    const uint32_t row_bytes = (extent.width * 3 + 3) & ~3u;  // BMP rows pad to 4 bytes
    const uint32_t image_bytes = row_bytes * extent.height;
    std::vector<uint8_t> bmp(54 + image_bytes, 0);
    const uint32_t file_size = uint32_t(bmp.size());
    // BITMAPFILEHEADER + BITMAPINFOHEADER (24-bit, bottom-up)
    bmp[0]='B'; bmp[1]='M';
    std::memcpy(&bmp[2], &file_size, 4);
    const uint32_t data_offset = 54; std::memcpy(&bmp[10], &data_offset, 4);
    const uint32_t hdr_size = 40;    std::memcpy(&bmp[14], &hdr_size, 4);
    const int32_t w = int32_t(extent.width), h = int32_t(extent.height);
    std::memcpy(&bmp[18], &w, 4); std::memcpy(&bmp[22], &h, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(&bmp[26], &planes, 2); std::memcpy(&bmp[28], &bpp, 2);
    std::memcpy(&bmp[34], &image_bytes, 4);
    for (uint32_t y = 0; y < extent.height; ++y)
    {
        uint8_t* row = bmp.data() + 54 + row_bytes * (extent.height - 1 - y);  // bottom-up
        const uint16_t* src_row = pixels + VkDeviceSize(y) * extent.width * 4;
        for (uint32_t x = 0; x < extent.width; ++x)
        {
            row[x * 3 + 0] = encode(src_row[x * 4 + 2]);  // B
            row[x * 3 + 1] = encode(src_row[x * 4 + 1]);  // G
            row[x * 3 + 2] = encode(src_row[x * 4 + 0]);  // R
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bmp.data()), std::streamsize(bmp.size()));
    out.close();
    allocator_.destroy_resource(staging);
    STRING_LOG_INFO("[capture] frame {} -> {}", frame_count_, path);
}

// The whole-frame entry point, called once per frame from Application::run.
void Renderer::draw()
{
    begin_frame();
    record_frame();
    end_frame();
    // STRING_SERIALIZE_FRAMES=1: drain the GPU after every frame (like the headless capture path).
    // Diagnostic for frames-in-flight hazards: if a live artifact vanishes with this set, it is a
    // cross-frame race on a shared (non-ringed) resource, not an algorithmic/over-cull bug.
    static const bool serialize = [] {
        const char* e = std::getenv("STRING_SERIALIZE_FRAMES");
        return e && e[0] == '1';
    }();
    if (serialize) vkDeviceWaitIdle(device_.get_device());
}

void Renderer::bind_composite_source()
{
    global_descriptor_table_.bind(color_attachment_, string::gpu::descriptor_type::TEXTURE);
    const uint32_t slot = global_descriptor_table_.get_binding_slot(color_attachment_, string::gpu::descriptor_type::TEXTURE);
    composite_pass_.set_source(global_descriptor_table_.get_set(), slot);
}

void Renderer::handle_resize(const String::View::Extent& extent)
{
    VkExtent2D vk_extent = {extent.width, extent.height};
    presenter_.resize(vk_extent);
    for (auto& pass : scene_passes_)
    {
        pass->resize(vk_extent);
    }

    // Release the old HDR target's bindless slot before it is destroyed, then re-allocate.
    global_descriptor_table_.unbind(color_attachment_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(color_attachment_);
    color_attachment_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // Re-bind the new HDR target into the table and refresh the composite pass's slot.
    bind_composite_source();

    // Recreate the multisampled scene targets at the new size.
    allocator_.destroy_resource(msaa_color_);
    msaa_color_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .samples = msaa_samples_,
    });

    allocator_.destroy_resource(msaa_depth_);
    msaa_depth_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_D32_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Brief 04d: SAMPLED so the two-phase min-resolve compute can texelFetch its MSAA samples
        // to build the HiZ pyramid mip-0 (min = farthest, reverse-Z conservative) between phase-1
        // and phase-2 opaque draws. Still 4x MSAA D32; only read, never storage-written.
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .samples = msaa_samples_,
    });

    // The swapchain images and attachments were just recreated — their old VkImage handles
    // (and tracked layouts) are stale.
    resource_states_.clear();
}

}  // namespace String
