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
#include <string/gpu/vk_check.hpp>
#include <string/core/cvar.hpp>
#include <string/core/png_writer.hpp>
#include <string/debug_draw.hpp>
#include <string/gpu/driver.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/graph_plan.hpp>
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
, resources_(allocator_, global_descriptor_table_)
// Shader hot-reload: a 1-thread pool (mtime scans + recompiles are light and serial), the watcher
// polled each frame, and the Slang compiler with a content-hash cache under the shaders dir. The
// shaders/ tree is the include/import search root.
, shader_jobs_(1)
, file_watcher_(shader_jobs_)
// Cache lives under the per-user cache dir (the resources/shaders tree may be read-only, e.g.
// the Nix store); it is content-hash keyed so a stale entry is simply never hit.
// `shadercache/` beside the shaders is the READ-ONLY cache a package ships (see
// docs/off-nix-build.md); it is what a -Dslang=disabled build runs from. Absent in a dev tree,
// which is harmless — it is only consulted after the writable cache misses.
, shader_compiler_(
    string::core::user_cache_dir("shaders"),
    { std::filesystem::path(application_info.resources_directory) / "shaders" },
    std::filesystem::path(application_info.resources_directory) / "shadercache")
, shader_registry_(device_, shader_compiler_, file_watcher_, shader_jobs_)
, composite_pass_(device_, allocator_, global_descriptor_table_, std::filesystem::path(application_info.resources_directory), presenter_.get_format(), shader_registry_)
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

    window_->register_resize_event_callback(std::bind(&Renderer::handle_resize, this, std::placeholders::_1));

    VkExtent2D extent = presenter_.get_extent();

    // Render targets. color_attachment_ is 1-sample: it's the MSAA resolve destination and the
    // texture the composite pass samples. msaa_color_ / msaa_depth_ are the multisampled targets
    // the scene passes actually render into.
    color_attachment_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Brief 09: + STORAGE so the post-processing compute chain can write bloom back into the
        // resolved HDR target in place (the composite and the capture writer then both see it).
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
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

    // Brief 16 M0: register the two stable render targets as `Imported` logical handles so their
    // physical backing is resolved through the ResourceRegistry (see image_of/view_of). Re-pointed
    // in handle_resize when these ids change.
    color_target_ = resources_.import_image(color_attachment_);
    depth_target_ = resources_.import_image(msaa_depth_);

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
    engine_context pass_context{
        device_,
        allocator_,
        global_descriptor_table_,
        resources_,
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
        resources_.transients(),
    };
    build_scene(plan, pass_context, extent);

    for (auto& frame : frames_)
    {
        frame.frame_id = 0;
    }

    init_gpu_profiler();
    gpu_timing_.init(device_, frames_in_flight_);
    GpuProfiler::set_global(&gpu_timing_);
    GraphIntrospect::set_global(&introspect_);
}

// Everything it takes to bring a scene's passes into existence and make them drawable. Called from
// initialize() for the first scene and from load_scene() for every one after — ONE code path, which
// is the entire point: a bespoke reload path would be a second implementation that has to agree with
// this one forever, and the sequence below is order-sensitive enough that it would not.
//
// The order is load-bearing:
//   1. the plan constructs passes (they upload via the transfer batch and claim bindless slots),
//   2. they learn the resolved HDR target's slot,
//   3. their declared transient needs get backed by the per-frame arena,
//   4. they join the graph's execution list,
//   5. uploads drain, then they size themselves to the current extent.
void Renderer::build_scene(const RenderPlan& plan, engine_context& ctx, VkExtent2D extent)
{
    // Brief 11 endgame: the app constructs its passes AND returns a re-runnable fluent author lambda.
    // The Renderer owns the passes (lifecycle) and re-runs the author on every graph recompile.
    {
        RenderPlan::Setup setup = plan.run(ctx);
        scene_passes_ = std::move(setup.passes);
        scene_author_ = std::move(setup.author);
    }

    // Rebind the composite's HDR source. It is bound at init and on resize, but NOT on a scene
    // load — and composite_pass_ is renderer-owned, so it is excluded from the scene_passes_ loop
    // below that does exactly this for everything else. Idempotent: a no-op when the target
    // survived the switch, and the fix when it did not.
    bind_composite_source();

    // Brief 09: the passes exist now — notify the ones that consume the resolved HDR target.
    {
        const uint32_t color_slot = global_descriptor_table_.get_binding_slot(
            color_attachment_, string::gpu::descriptor_type::TEXTURE);
        for (auto& pass : scene_passes_)
            pass->bind_color_source(color_slot, color_attachment_);
    }

    // Brief 04e M3: all passes have declared their per-frame scratch needs; back them with one
    // device-local arena per frame slot (logged as the VRAM consolidation number).
    resources_.materialize_transients(frames_in_flight_);

    // Composite resolves the offscreen HDR target to the swapchain: reads color_attachment_,
    // writes the screen. Declared here (composite_pass_ is renderer-built, not in the plan).
    composite_pass_.usages = {
        { string::gpu::COLOR_TARGET,     Access::SampledRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT },
        { string::gpu::SWAPCHAIN_TARGET, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
    };

    // The frame graph's execution list: scene passes, then the composite resolve. written_
    // resources_ = everything the graph writes, so record_frame only transitions those (never
    // the static uploaded textures/buffers the transfer batch already left in SHADER_READ).
    frame_passes_.clear();
    for (auto& pass : scene_passes_)
        frame_passes_.push_back(pass.get());
    frame_passes_.push_back(&composite_pass_);
    // written_resources_ is recomputed per frame in record_frame (brief 04e M2): usages may name
    // per-frame-slot resources, so the written set is frame state, not init state.

    // Drain the async upload ring: submit any pending batch and wait for every in-flight batch
    // to finish (freeing all staging) before the first frame draws the uploaded resources.
    transfer_batch_.wait_idle();

    for (auto& pass : scene_passes_)
        pass->resize(extent);

    // The pass set changed, so the cached plan describes a graph that no longer exists.
    graph_dirty_ = true;
}

// Swap the running scene for another one's passes. The app decides WHEN (a UI click, a console
// command); the renderer owns the mechanics.
//
// SYNCHRONOUS AND STALLING, deliberately. Destroying a pass frees images and buffers the GPU may
// still be reading from frames already submitted, so there is no correct way to do this without
// first waiting for the device to go idle. A scene load is a multi-second operation dominated by
// glTF parsing and texture streaming anyway; hiding a device wait inside that is not the problem.
// Loading asynchronously (build the new passes, then swap) would avoid the hitch but doubles peak
// VRAM — the old scene is still resident while the new one uploads — which is the worse trade on
// the machine this has to run on.
void Renderer::load_scene(const RenderPlan& plan)
{
    STRING_LOG_INFO("Loading scene...");
    const auto t0 = std::chrono::steady_clock::now();

    // Drain the upload ring BEFORE waiting on the device. The transfer batch can hold copies that
    // are RECORDED BUT NOT YET SUBMITTED, targeting buffers the outgoing passes own; a device wait
    // does not flush those, so they would be submitted after their destination was destroyed.
    transfer_batch_.wait_idle();
    // Now nothing in flight may still reference what is about to be destroyed.
    ::string::gpu::vk_report(vkDeviceWaitIdle(device_.get_device()), "vkDeviceWaitIdle(load_scene teardown)");

    // Drop the passes FIRST: their destructors unbind bindless slots and destroy their resources, and
    // they must do that while the allocator, table and device are all still alive (the same ordering
    // constraint the destructor has). frame_passes_ holds raw pointers into scene_passes_, so it has
    // to be cleared in the same breath or it dangles.
    frame_passes_.clear();
    scene_author_ = nullptr;
    compiled_frame_ = {};
    scene_passes_.clear();

    // Anything a destroyed pass deferred rather than freed outright.
    for (auto& frame : frames_)
        frame.garbage_collector.flush();

    // The rolling per-pass timings describe passes that no longer exist — without this the HUD and
    // the frametime log line show the outgoing and incoming scenes' passes mixed together.
    gpu_timing_.reset_stats();

    // The per-frame scratch arena is sized from what the OLD passes reserved at construction, and
    // reserve() is construction-time-only. Free it and forget the reservations so the incoming
    // passes size a fresh one — otherwise the second scene's first reserve() trips
    // "reserve() after materialize()".
    resources_.reset_transients();

    // Every image the outgoing scene owned has just been destroyed. The tracker keys images by RAW
    // VkImage HANDLE, and the driver reuses handle values — so without this the incoming scene's
    // fresh images inherit the dead scene's tracked layout. The consequences are silent and fatal:
    // transition()'s "pure read in the current layout" fast path (resource_state.cpp) sees a layout
    // that already matches and emits NO barrier at all, so the first frame of the new scene samples
    // an image that is really still UNDEFINED. On a driver that compresses colour targets (RDNA2's
    // DCC) that means reading compression metadata which was never initialised — garbage output, and
    // a shader fault that takes the queue down with it. handle_resize has always done this, for the
    // same reason and with the same comment; load_scene destroys strictly more and did not.
    resource_states_.clear();

    const VkExtent2D extent = presenter_.get_extent();
    engine_context ctx{
        device_,
        allocator_,
        global_descriptor_table_,
        resources_,
        shader_registry_,
        transfer_batch_,
        window_->get_input(),
        input_map_,
        string::gpu::COLOR_TARGET,
        string::gpu::DEPTH_TARGET,
        std::filesystem::path(application_info_.resources_directory),
        static_cast<uint16_t>(frames_in_flight_),
        msaa_samples_,
        &gpu_profiler_ctx_,
        resources_.transients(),
    };
    build_scene(plan, ctx, extent);

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    STRING_LOG_INFO("Scene loaded in {:.0f} ms ({} passes)", ms, scene_passes_.size());
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
    ::string::gpu::vk_report(vkDeviceWaitIdle(device_.get_device()), "vkDeviceWaitIdle(build_scene)");

    // Tear down the per-lane Tracy GPU contexts (no-op when profiling is off) before the device
    // goes away. gpu_profiler_ctx_ aliases the main lane's context — don't double-destroy it.
    for (STRING_PROFILE_GPU_CONTEXT_TYPE& ctx : lane_profiler_ctxs_)
        STRING_PROFILE_GPU_CONTEXT_DESTROY(ctx)
    lane_profiler_ctxs_.clear();
    gpu_profiler_ctx_ = nullptr;
    GpuProfiler::set_global(nullptr);
    GraphIntrospect::set_global(nullptr);
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
    // The transient arena is owned + freed by the ResourceRegistry dtor now (brief 16 M5).
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
    // Brief 09: the composite is renderer-owned (not in scene_passes_) but now has real per-frame
    // work — the output-transform LUT bake (first frame) / re-bake on grading CVar change.
    composite_pass_.update(delta_time, static_cast<uint16_t>(current_frame_));
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
    // Bounded, not UINT64_MAX, purely so a stall is DIAGNOSABLE. These two cases look identical from
    // the outside (frozen window, black screen, driver reset) but have opposite causes:
    //
    //   - this wait TIMES OUT  -> the CPU is blocked on a timeline value the GPU never signalled:
    //                             a missed/mismatched signal, i.e. a synchronisation bug of ours.
    //   - this wait RETURNS ok -> the CPU is running fine and the GPU is chewing on work that never
    //                             finishes: a shader hang / oversized dispatch, and the driver's
    //                             watchdog is what kills us.
    //
    // VK_EXT_device_fault reports "no fault" for BOTH, so this is the cheapest way to tell them
    // apart on a machine we cannot attach a debugger to. Five seconds is well past the ~2s Windows
    // TDR threshold, so a real hang trips TDR first and this only fires on a genuine CPU-side stall.
    constexpr uint64_t kFrameWaitTimeoutNs = 5'000'000'000ull;
    if (const VkResult wr = vkWaitSemaphores(device_.get_device(), &wait_info, kFrameWaitTimeoutNs);
        wr == VK_TIMEOUT)
    {
        uint64_t reached = 0;
        vkGetSemaphoreCounterValue(device_.get_device(), frame_semaphore_, &reached);
        STRING_LOG_CRITICAL("FRAME WAIT STALLED: waiting for main timeline value {} but the GPU has "
                            "only reached {} after 5s. The CPU is blocked on a signal that never "
                            "came — a synchronisation bug, NOT a GPU hang (a hang would trip the "
                            "driver watchdog instead). frame_count={} slot={}",
                            frame.frame_id, reached, frame_count_, current_frame_);
        // EVERY lane, not just main. A stall here is nearly always a cross-lane edge: the main
        // submit waits on another lane's timeline at a value that lane never signalled, so main
        // never starts and its own timeline never advances. Printing only the main value shows
        // the symptom and hides the cause.
        for (uint32_t i = 0; i < submissions_.lane_count(); ++i)
        {
            uint64_t v = 0;
            vkGetSemaphoreCounterValue(device_.get_device(), submissions_.timeline(i), &v);
            STRING_LOG_CRITICAL("  lane '{}': timeline reached {}, last value we submitted was {}",
                                submissions_.lane(i).name, v, submissions_.last_signaled(i));
        }

        // Do NOT fall through. Everything below this point — flushing the frame's garbage collector,
        // resetting the frame slot's command pool, acquiring with this slot's binary semaphore —
        // assumes the GPU is DONE with this slot. On a timeout it demonstrably is not, so proceeding
        // destroys resources that are still being read, resets a command buffer that is still
        // pending, and reuses a semaphore that still has operations outstanding. That is what turns a
        // recoverable stall into VK_ERROR_DEVICE_LOST a few frames later, with the original cause
        // long gone from the log. Keep waiting instead: either the work lands and we continue
        // correctly, or the device is genuinely lost and we get told so.
        const VkResult wr2 = vkWaitSemaphores(device_.get_device(), &wait_info, UINT64_MAX);
        ::string::gpu::vk_report(wr2, "vkWaitSemaphores(frame pacing, after stall)");
    }
    else
    {
        ::string::gpu::vk_report(wr, "vkWaitSemaphores(frame pacing)");
    }

    frame.garbage_collector.flush();
    main_recorder(current_frame_).reset();

    // Acquire the swapchain image for this frame, unconditionally and exactly once.
    //
    // This used to be LATE-LATCHED — deferred into record_frame until the composite group needed
    // it — to buy CPU run-ahead so the async-compute submit could overlap the previous frame. That
    // optimisation cost far more than it bought. end_frame's main submit ALWAYS waits on
    // acquired_wait_semaphore_ and record_frame ALWAYS transitions acquired_image_ to Present, so
    // the whole design rested on an invariant ("the composite group always runs") that was implicit,
    // unenforced, and false often enough to matter. When it broke, the frame waited on a BINARY
    // semaphore belonging to an earlier frame that had already been waited on and consumed — a wait
    // that can never be satisfied. The submit then sits in the queue forever: the CPU runs ahead,
    // the GPU stalls on the oldest submitted frame, and begin_frame blocks with
    //   "waiting for main timeline value N but the GPU has only reached N-1"
    // while VK_EXT_device_fault reports no fault, because nothing is hung — it is waiting.
    //
    // Acquiring here makes that unrepresentable: one acquire, one fresh semaphore, always waited,
    // always presented. No idempotence flag, no conditional wait lists, no frame that presents an
    // image it never drew. The lost overlap is recoverable later (and is worth measuring before
    // being reintroduced); correctness is not negotiable in the same way.
    {
        string::gpu::acquired_image acquired = presenter_.acquire_next_frame();
        acquired_image_ = acquired.image;
        acquired_image_view_ = acquired.image_view;
        acquired_wait_semaphore_ = acquired.wait_for_image_available;
        acquired_signal_semaphore_ = acquired.signal_when_ready_to_present;
    }

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

    // NOTE: the swapchain acquire happens LATE, inside record_frame() just before the composite
    // group — not here. Acquiring at the top of the frame blocked the CPU behind the previous
    // frame's present and capped run-ahead at ~1 frame (the 04e M4 overlap miss). The per-slot
    // pacing throttle is the timeline wait above, not the acquire.
}

void Renderer::rebuild_execution_plan()
{
    // Brief 11 endgame: RE-AUTHOR the persistent FrameGraph from the app's fluent author (which declares
    // each pass's I/O + flags + callbacks, referencing the constructed passes) plus the renderer-owned
    // composite resolve, then compile (toposort + lifetime/aliasing + toggle survival). Runs on
    // invalidation only (toggle flip / resize). The app author runs AFTER update() so usagesFrom() sees
    // populated per-frame usages; the executor then drives the CompiledFrame's PassExec callbacks.
    frame_graph_ = FrameGraph{};
    if (scene_author_) scene_author_(frame_graph_);
    // Composite resolve (offscreen HDR -> swapchain) is renderer-owned, authored last so it forms the
    // final swapchain group.
    frame_graph_.pass("composite")
        .usages([this]() -> const std::vector<ResourceUsage>& { return composite_pass_.usages; })
        .record([this](string::gpu::pass_context& ctx) { composite_pass_.record(ctx.rec, ctx.frame_slot); })
        .finish();
    compiled_frame_ = frame_graph_.compile();
    graph_dirty_ = false;
    refresh_introspection();

    // One-shot lifetime report (the planner's per-resource [first,last] topo span = the transient-
    // aliasing input). One-shot across recompiles to avoid re-logging on every toggle/resize.
    static bool logged_lifetimes = false;
    if (!logged_lifetimes)
    {
        logged_lifetimes = true;
        for (const auto& [id, lifetime] : compiled_frame_.plan.resource_lifetimes)
            STRING_LOG_INFO("[graph] resource {}: passes [{}..{}] first_writer={}",
                            id, lifetime.first.value_or(0), lifetime.last.value_or(0),
                            lifetime.first_writer.has_value()
                                ? static_cast<int64_t>(*lifetime.first_writer) : -1);
    }
}

void Renderer::refresh_introspection()
{
    // Brief 14 M2: refill the debug snapshot. Runs on RECOMPILE only — the plan is the only thing
    // any of this reads, and it changes on toggle flip / resize, not per frame.
    introspect_.passes.clear();
    introspect_.resources.clear();
    introspect_.dropped.clear();
    introspect_.passes.reserve(compiled_frame_.passes.size());
    for (const CompiledPass& p : compiled_frame_.passes)
        introspect_.passes.push_back({ p.name, {}, {}, 0 });

    // RESOURCE LABELS ARE DERIVED, NOT STORED. Neither the registry nor the allocator keeps a debug
    // string, and adding one would thread string-carrying surface through ~26 declaration sites of a
    // system that has no other opinion about names — for a debug panel's row headers. So a row is
    // identified by WHAT PRODUCED IT, which for a lifetime chart is the honest identity anyway: the
    // view is about production and consumption. `#n` disambiguates several resources written by the
    // same pass, by their stable order in the lifetime map.
    //
    // If this turns out to be ambiguous in real use, add real names THEN — with evidence about which
    // resources were actually confusable.
    std::unordered_map<std::uint32_t, int> per_writer;
    for (const ResourceInfo& r : resources())
    {
        GraphIntrospect::Resource out;
        out.is_image = r.is_image;
        out.first = r.first_pass.value_or(0);
        out.last = r.last_pass.value_or(out.first);
        out.first_writer = r.first_writer;

        if (r.id == string::gpu::SWAPCHAIN_TARGET)   out.label = "swapchain";
        else if (r.id == string::gpu::COLOR_TARGET)  out.label = "color target";
        else if (r.id == string::gpu::DEPTH_TARGET)  out.label = "depth target";
        else if (r.first_writer.has_value() && *r.first_writer < introspect_.passes.size())
        {
            const int n = per_writer[*r.first_writer]++;
            out.label = introspect_.passes[*r.first_writer].name;
            if (n > 0) out.label += " #" + std::to_string(n);
        }
        else
        {
            // Nobody in this frame writes it: an external input, or a resource whose producer was
            // toggled off. Either way "who made this" has no answer, and saying so is more useful
            // than a raw id pretending to be a name.
            out.label = "external";
        }
        introspect_.resources.push_back(std::move(out));
    }

    std::sort(introspect_.resources.begin(), introspect_.resources.end(),
              [](const GraphIntrospect::Resource& a, const GraphIntrospect::Resource& b) {
                  if (a.first != b.first) return a.first < b.first;
                  return a.last < b.last;
              });

    // --- M3: edges, per-pass usages, layer depth ------------------------------------------------
    //
    // The resource rows are sorted above, so build the id -> row map AFTER the sort; a Use recorded
    // against a pre-sort index would point at an unrelated resource.
    std::unordered_map<string::gpu::resource_id, std::uint32_t> row_of;
    row_of.reserve(introspect_.resources.size());
    {
        std::vector<ResourceInfo> infos = resources();
        std::sort(infos.begin(), infos.end(), [](const ResourceInfo& a, const ResourceInfo& b) {
            const std::uint32_t af = a.first_pass.value_or(0), bf = b.first_pass.value_or(0);
            if (af != bf) return af < bf;
            return a.last_pass.value_or(af) < b.last_pass.value_or(bf);
        });
        for (std::uint32_t i = 0; i < infos.size(); ++i) row_of[infos[i].id] = i;
    }

    for (std::size_t i = 0; i < compiled_frame_.passes.size(); ++i)
    {
        for (const ResourceUsage& u : compiled_frame_.passes[i].usages)
        {
            const auto it = row_of.find(u.key());
            if (it != row_of.end())
                introspect_.passes[i].uses.push_back({ it->second, is_write(u.access) });
        }
    }

    // `plan.adjacency` is indexed by PLANNER position; everything the UI sees is indexed by COMPILED
    // (toposorted) position. `toposorted[compiled] = planner` is the mapping, so invert it. Getting
    // this wrong would draw a graph whose edges connect the wrong passes — and it would look
    // plausible, which is the dangerous kind of wrong.
    std::unordered_map<std::uint32_t, std::uint32_t> compiled_of;
    const std::vector<std::uint32_t>& topo = compiled_frame_.plan.toposorted;
    for (std::uint32_t c = 0; c < topo.size(); ++c) compiled_of[topo[c]] = c;

    for (std::uint32_t c = 0; c < topo.size() && c < introspect_.passes.size(); ++c)
    {
        const std::uint32_t planner = topo[c];
        if (planner >= compiled_frame_.plan.adjacency.size()) continue;
        for (std::uint32_t succ_planner : compiled_frame_.plan.adjacency[planner])
        {
            const auto it = compiled_of.find(succ_planner);
            if (it != compiled_of.end()) introspect_.passes[c].successors.push_back(it->second);
        }
    }

    // Longest-path depth. A single forward sweep suffices because the passes are already
    // toposorted: every predecessor of `c` has its final depth before `c` is read.
    for (std::size_t c = 0; c < introspect_.passes.size(); ++c)
        for (std::uint32_t s : introspect_.passes[c].successors)
            if (s < introspect_.passes.size())
                introspect_.passes[s].depth =
                    std::max(introspect_.passes[s].depth, introspect_.passes[c].depth + 1);

    // Authored but not compiled: toggled off, or transitively skipped by the `.requires()` fixpoint.
    // Both read the same here, which is the honest answer — from the plan's side "why" is one fact:
    // it isn't in the frame.
    for (const PassInfo& a : frame_graph_.passes())
    {
        const bool compiled = std::any_of(
            introspect_.passes.begin(), introspect_.passes.end(),
            [&](const GraphIntrospect::Pass& p) { return p.name == a.name; });
        if (!compiled) introspect_.dropped.emplace_back(a.name);
    }
}

std::vector<Renderer::ResourceInfo> Renderer::resources() const
{
    // Brief 11 M4 introspection: every resource the compiled plan touches, with its topo lifetime
    // span (the aliasing input the planner already computed). is_image is best-effort — the render-
    // target sentinels + registry IMAGE handles (usage.key() sets IMAGE_HANDLE_BASE); buffers + raw
    // ids read as non-image. The target visualizer (brief 14) filters on it.
    std::vector<ResourceInfo> out;
    out.reserve(compiled_frame_.plan.resource_lifetimes.size());
    for (const auto& [id, lt] : compiled_frame_.plan.resource_lifetimes)
    {
        const bool is_image =
            id == string::gpu::SWAPCHAIN_TARGET || id == string::gpu::COLOR_TARGET
            || id == string::gpu::DEPTH_TARGET || (id & ResourceUsage::IMAGE_HANDLE_BASE) != 0;
        out.push_back({ id, is_image, lt.first, lt.last, lt.first_writer });
    }
    return out;
}

void Renderer::record_frame()
{
    STRING_PROFILE_SCOPE("record_frame")
    string::gpu::command_recorder& recorder = main_recorder(current_frame_);
    VkCommandBuffer command_buffer = recorder.begin();
    // Brief 16 M1: the execute-time surface handed to every main-queue pass callback this frame
    // (record / record_compute / inline-async). It resolves a pass's logical handles to this frame's
    // physical backing (frame_slot = current_frame_).
    string::gpu::pass_context frame_ctx{ recorder, resources_, static_cast<std::uint32_t>(current_frame_) };
    // Brief 16: the executor is the SINGLE resource-resolution authority. A pass declares each usage
    // with a LOGICAL registry handle (or a raw sentinel/persistent id); `res()` resolves it to this
    // frame's physical id here — passes no longer pre-resolve resources->physical() into their usages.
    const auto res = [&](const ResourceUsage& u) {
        return u.resolve(resources_, static_cast<std::uint32_t>(current_frame_));
    };
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

    // Brief 11 step 1: the persistent frame plan. The FrameGraph is compiled ONCE (toposort +
    // lifetimes) and cached; this frame re-records cheaply from execution_order_ instead of
    // rebuilding the graph. Recompile only on invalidation — a per-pass enable/disable flip (the
    // enabled-signature below changes) or a forced dirty (first frame / resize). The order is stable
    // otherwise (resource ids don't vary across frames or resize), and ALL inter-pass barriers still
    // derive from live pass->usages through resource_states_ — pass-declared usage remains the single
    // source of truth for sync — so per-frame-slot buffer variation needs no re-plan.
    uint64_t sig = 1469598103934665603ull;
    for (Pass* pass : frame_passes_)
        sig = (sig ^ (pass->is_enabled() ? 1u : 0u)) * 1099511628211ull;
    if (graph_dirty_ || sig != enabled_signature_)
    {
        enabled_signature_ = sig;
        rebuild_execution_plan();
    }
    // Brief 11 fluent migration: the executor drives the CompiledFrame's PassExec surface (callbacks +
    // flags + live-usage pointers), NOT Pass* virtuals — the `source` bridge is retired. Each pass's
    // live per-frame usages are read through cp.exec.usages (a stable pointer into the pass object).
    const std::vector<CompiledPass>& order = compiled_frame_.passes;

    // The set of resources the graph itself writes this frame (usages may name per-frame-slot
    // resources, so recomputed live each frame). Static uploaded inputs are never re-transitioned.
    written_resources_.clear();
    for (const CompiledPass& cp : order)
        for (const ResourceUsage& usage : cp.exec.usages())
            if (is_write(usage.access))
                written_resources_.insert(res(usage));

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
        std::vector<const CompiledPass*> async_passes;
        for (const CompiledPass& cp : order)
            if (cp.exec.has_async && cp.exec.has_async()) async_passes.push_back(&cp);
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
            string::gpu::pass_context async_pass_ctx{ async_recorder, resources_,
                                                      static_cast<std::uint32_t>(current_frame_) };
            std::vector<VkBufferMemoryBarrier2> releases;
            std::vector<VkBufferMemoryBarrier2> acquires;
            VkPipelineStageFlags2 wait_stages = 0;
            for (const CompiledPass* cp : async_passes)
            {
                {
                    STRING_PROFILE_GPU_ZONE_DYNAMIC(async_ctx, async_cb, "froxel-cull")
                    cp->exec.record_async(async_pass_ctx);
                }
                for (const ResourceUsage& usage : cp->exec.async_usages())
                {
                    const AccessScope scope = access_scope(usage.access);
                    const VkBuffer buffer = allocator_.get_buffer(res(usage)).buffer;
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
            if (const VkResult r = vkQueueSubmit2(lane.vk_queue, 1, &async_submit, nullptr);
                !::string::gpu::vk_report(r, "vkQueueSubmit2(async compute)"))
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
            for (const CompiledPass* cp : async_passes)
            {
                for (const ResourceUsage& usage : cp->exec.async_usages())
                    if (is_write(usage.access))
                        resource_states_.buffer_access(res(usage), usage.access, usage.stage);
                resource_states_.flush_buffers(command_buffer);
                {
                    STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, "froxel-cull")
                    gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_),
                                            cp->name + " (async-inline)");
                    cp->exec.record_async(frame_ctx);
                    gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
                }
                // The main-queue reads: noted now, flushed with the next barrier point (before
                // any graphics group begins).
                for (const ResourceUsage& usage : cp->exec.async_usages())
                    if (!is_write(usage.access))
                        resource_states_.buffer_access(res(usage), usage.access, usage.stage);
            }
        }
    }

    // Compute prepass: passes may dispatch GPU work (e.g. GPU culling that fills indirect
    // worklists) outside dynamic rendering, before any graphics group. Each pass's declared
    // compute-stage buffer usages derive the barriers it needs (vs prior tracked accesses).
    // Brief 09: compute_only passes are excluded — they execute whole at their toposorted
    // position in the group loop below (post-processing must run AFTER the scene resolve),
    // and their declared usages are processed there, not here.
    for (const CompiledPass& cp : order)
    {
        if (cp.exec.compute_only) continue;
        for (const ResourceUsage& usage : cp.exec.usages())
            if (is_buffer_usage(usage) && is_compute_stage(usage.stage))
                resource_states_.buffer_access(res(usage), usage.access, usage.stage);
        resource_states_.flush_buffers(command_buffer);

        const std::string& pass_name = cp.name;
        STRING_PROFILE_SCOPE_DYNAMIC(pass_name.data(), pass_name.size())
        STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, pass_name.c_str())
        gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_), pass_name + " (cs)");
        if (cp.exec.record_compute) cp.exec.record_compute(frame_ctx);
        gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
    }

    // The color / depth target a pass renders into (every frame pass writes exactly one color
    // target; string::gpu::SWAPCHAIN_TARGET means the screen).
    // color/depth attachments are always renderer sentinels (COLOR/DEPTH/SWAPCHAIN_TARGET), never
    // registry handles, so they carry a raw `resource` — no per-frame resolution needed here.
    const auto color_target_of = [](const CompiledPass& cp) -> string::gpu::resource_id {
        for (const ResourceUsage& usage : cp.exec.usages())
            if (usage.access == Access::ColorWrite) return usage.resource;
        return string::gpu::SWAPCHAIN_TARGET;
    };
    const auto depth_target_of = [](const CompiledPass& cp) -> std::optional<string::gpu::resource_id> {
        for (const ResourceUsage& usage : cp.exec.usages())
            if (usage.access == Access::DepthWrite) return usage.resource;
        // Depth READS must also bind the depth image as an attachment: depth-tested/write-off
        // pipelines (debug lines) need it, and — critically — the reopened group that hosts a
        // breaker's phase-2 draws inherits THIS group's depth attachment. Matching only DepthWrite
        // left that group with pDepthAttachment = NULL, so the phase-2 opaque + sorted-transparency
        // draws ran with NO depth test at all: every disoccluded meshlet landed on top of the scene
        // (far geometry over near = persistent semi-transparent surfaces, popping under motion,
        // clean with HiZ off because single-pass never splits the group).
        for (const ResourceUsage& usage : cp.exec.usages())
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
    // Brief 04e follow-up (late-latch acquire): the swapchain image is acquired HERE, mid-record,
    // at the first point the frame actually needs it — the composite group, which renders straight
    // into the swapchain image view. Everything recorded before that point (async submit, compute
    // prepass, all offscreen groups) touches only non-swapchain resources, so moving the blocking
    // vkAcquireNextImageKHR off the top of the frame restores CPU run-ahead: the async-compute
    // submit for frame N is already on its queue before the CPU can block here, letting it overlap
    // frame N-1's main-queue work instead of landing in the inter-frame idle gap. If acquire hits
    // OUT_OF_DATE, the presenter recreates the swapchain (device-idle) and retries internally —
    // the partially recorded command buffer stays valid because nothing recorded so far references
    // the swapchain, and the fresh image/view is latched before any swapchain command is recorded.
    // Copy the handles out of string::gpu::acquired_image (which holds references into vectors
    // resize() reallocates).
    // The swapchain image was acquired unconditionally in begin_frame — see the note there for why
    // the late-latch version was removed. acquired_image_ / _view_ / the two semaphores are already
    // valid and belong to THIS frame.

    size_t start = 0;
    bool seen_msaa_group = false;   // has a COLOR_TARGET group already cleared the MSAA targets?
    while (start < order.size())
    {
        // Brief 11 step 3: prepass-only compute passes (IBL update, GTAO, ...) already ran in the
        // frame-top compute prepass; they draw nothing and form no group, so skip them here.
        if (order[start].exec.prepass_only) { ++start; continue; }
        // Brief 09: a compute_only pass executes standalone, OUTSIDE any rendering group, at its
        // toposorted position (the post chain runs here: after the last MSAA group's resolve into
        // COLOR_TARGET, before the composite group samples it). Its barriers derive from its
        // declared usages exactly like a group's would: image usages transition through the
        // tracker (StorageImageWrite -> GENERAL covers the sample+storage-write mix), buffer
        // usages merge into one flushed memory barrier. Its own transient chains (bloom mips)
        // are intra-pass state with local barriers — the documented allowed class.
        if (order[start].exec.compute_only)
        {
            const CompiledPass& cp = order[start];
            std::unordered_map<string::gpu::resource_id, ResourceUsage> compute_transitions;
            for (const ResourceUsage& usage : cp.exec.usages())
            {
                if (is_buffer_usage(usage))
                {
                    resource_states_.buffer_access(res(usage), usage.access, usage.stage);
                    continue;
                }
                // Transition graph-WRITTEN images (this pass's outputs) AND any already-TRACKED image
                // it reads — e.g. hiz.build's SampledRead of the seeded hz.depth resolve target, which
                // is not in written_resources_ (nothing declares it a write) but must derive its
                // wait-on-resolve. Static uploaded inputs are neither written nor tracked -> skipped.
                if (!written_resources_.contains(res(usage))
                    && !resource_states_.is_tracked(image_of(res(usage)))) continue;
                auto [it, inserted] = compute_transitions.try_emplace(res(usage), usage);
                if (!inserted && is_write(usage.access) && !is_write(it->second.access))
                    it->second = usage;
            }
            for (const auto& [resource, usage] : compute_transitions)
            {
                const auto [aspect, mips] = image_meta(resource);
                resource_states_.transition(command_buffer, image_of(resource),
                    aspect, usage.access, usage.stage, /*discard=*/false, mips);
            }
            resource_states_.flush_buffers(command_buffer);

            const std::string& pass_name = cp.name;
            STRING_PROFILE_SCOPE_DYNAMIC(pass_name.data(), pass_name.size())
            STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, pass_name.c_str())
            gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_), pass_name);
            cp.exec.record(frame_ctx);
            gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
            ++start;
            continue;
        }
        const string::gpu::resource_id group_color = color_target_of(order[start]);
        // First (and only) group targeting the screen: latch the swapchain image now, before the
        // group's transitions/attachment info dereference image_of/image_view_of(SWAPCHAIN_TARGET).
        // (swapchain image already acquired in begin_frame)
        std::optional<string::gpu::resource_id> group_depth;
        size_t end = start;
        while (end < order.size() && color_target_of(order[end]) == group_color)
        {
            if (auto depth = depth_target_of(order[end])) group_depth = depth;
            ++end;
        }

        // Is this the LAST COLOR_TARGET (MSAA) group in the chain? Only the last one resolves; the
        // rest STORE their MSAA samples for the following group to LOAD. A group is the last MSAA
        // group iff no LATER pass also targets COLOR_TARGET. Brief 11 step 2: look PAST intervening
        // compute_only passes — the HiZ build (geometry.phase1 -> hiz.build -> geometry.phase2) breaks
        // group contiguity in execution_order, but the MSAA chain resumes at phase2 after it. Without
        // this skip, phase1's group would see the compute-only hiz next, think it is last, and resolve
        // color BEFORE phase2 draws — the disocclusion complement would be lost (04d class).
        const bool this_is_msaa = (group_color == string::gpu::COLOR_TARGET);
        size_t next_color = end;
        while (next_color < order.size() && order[next_color].exec.compute_only)
            ++next_color;
        const bool next_is_msaa = this_is_msaa && next_color < order.size()
            && color_target_of(order[next_color]) == string::gpu::COLOR_TARGET;
        const bool msaa_is_first = this_is_msaa && !seen_msaa_group;   // clears vs loads
        const bool msaa_is_last = this_is_msaa && !next_is_msaa;       // resolves vs stores
        if (this_is_msaa) seen_msaa_group = true;

        // Brief 04d: a group MIN-resolves its MSAA depth into a single-sample resolve target (reverse-Z:
        // min = farthest = conservative HiZ occluder), so the following HiZ build can read a normally-
        // samplable depth. Brief 11 P2 (B1): the target is named by a declared Access::DepthResolve
        // usage (scanned here) rather than the removed Pass::depth_resolve_target() hook. 0 = no resolve.
        string::gpu::resource_id depth_resolve = 0;
        for (size_t i = start; i < end; ++i)
            for (const ResourceUsage& usage : order[i].exec.usages())
                if (usage.access == Access::DepthResolve) depth_resolve = res(usage);

        // Barriers: transition each graph-written image the group touches to the state its usage
        // needs (deduped per resource). Static uploaded inputs are skipped — they aren't in
        // written_resources_, and buffer usages map to VK_IMAGE_LAYOUT_UNDEFINED. Reads of a
        // graph-written resource (the composite sampling color) get the correct source layout
        // from the tracker; writes discard the previous contents.
        std::unordered_map<string::gpu::resource_id, ResourceUsage> group_transitions;
        for (size_t i = start; i < end; ++i)
            for (const ResourceUsage& usage : order[i].exec.usages())
            {
                // Graphics-phase BUFFER usages (indirect worklists, task/fragment storage reads,
                // the visibility bitfield's task-stage RMW): note them with the tracker so their
                // barriers — vs this frame's compute writes AND the previous frame's draws —
                // derive from the declaration. Flushed as one merged barrier before rendering.
                if (is_buffer_usage(usage))
                {
                    if (!is_compute_stage(usage.stage))
                        resource_states_.buffer_access(res(usage), usage.access, usage.stage);
                    continue;
                }
                if (!written_resources_.contains(res(usage))) continue;
                // Writes take precedence: a group that both writes and reads a resource as an
                // attachment must sit in the WRITE layout for the whole rendering (the group's
                // attachment info uses ATTACHMENT_OPTIMAL; attachment reads are legal there). A
                // read-usage last-wins here once put depth in READ_ONLY under a group whose
                // rendering declared ATTACHMENT_OPTIMAL -> per-frame validation errors.
                auto [it, inserted] = group_transitions.try_emplace(res(usage), usage);
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
            // Brief 11 step 2b: transition every mip (e.g. geometry.phase2's SampledRead of the whole
            // HiZ pyramid), not just mip 0 — image_meta resolves the chain length.
            const uint32_t mips = image_meta(resource).second;
            resource_states_.transition(command_buffer, image_of(resource), aspect,
                usage.access, usage.stage, /*discard=*/is_write(usage.access) && !load_depth, mips);
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

        // Brief 16 M4 (framework-opens): the framework derives every attachment op from the group's
        // lifetime facts + opens the render pass. (Step 2 already moved render-pass ownership to the
        // executor; this centralizes the MSAA clear/load/store/resolve chain that was left inline here.)
        open_group_rendering(command_buffer, group_color, group_depth, depth_resolve,
            msaa_group, msaa_is_first, msaa_is_last, extent, viewport, scissor);

        // Brief 11 step 2: geometry.phase1 / hiz.build / geometry.phase2 are ordinary registered
        // passes now — phase2 records here as the first pass of the reopened MSAA group, hiz.build ran
        // standalone as a compute_only pass between the two groups. The old breaks_scene_group /
        // record_between / record_after_between / pending_after_between machinery is retired.
        for (size_t i = start; i < end; ++i)
        {
            const std::string& pass_name = order[i].name;
            STRING_PROFILE_SCOPE_DYNAMIC(pass_name.data(), pass_name.size())
            STRING_PROFILE_GPU_ZONE_DYNAMIC(gpu_profiler_ctx_, command_buffer, pass_name.c_str())
            gpu_timing_.write_begin(command_buffer, static_cast<uint32_t>(current_frame_), pass_name);
            order[i].exec.record(frame_ctx);
            gpu_timing_.write_end(command_buffer, static_cast<uint32_t>(current_frame_));
        }

        vkCmdEndRendering(command_buffer);

        // Brief 11 step 2b: this group just MIN-resolved its MSAA depth into `depth_resolve` (hz.depth)
        // at EndRendering — a write in the COLOR_ATTACHMENT_OUTPUT stage the tracker can't observe.
        // Seed it so the following hiz.build pass's SampledRead derives the correct wait-on-resolve
        // (replacing record_hiz's old hand-rolled DEPTH_ATTACHMENT->SHADER_READ barrier).
        if (depth_resolve != 0)
            resource_states_.seed(image_of(depth_resolve),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        start = end;
    }

    // The swapchain image was acquired in begin_frame and rendered by the final group; ready it
    // for presentation. No acquire here: exactly one acquire per frame, done up front.

    resource_states_.transition(command_buffer, acquired_image_, VK_IMAGE_ASPECT_COLOR_BIT,
        Access::Present, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

    // Collect this frame's GPU timestamp queries into the command buffer (once per frame, on the
    // still-recording buffer). Non-blocking — Tracy reads back completed queries opportunistically
    // across the frames-in-flight ring. No-op without -Dtracy.
    STRING_PROFILE_GPU_COLLECT(gpu_profiler_ctx_, command_buffer)

    recorder.end();
}

void Renderer::open_group_rendering(VkCommandBuffer command_buffer, string::gpu::resource_id group_color,
    const std::optional<string::gpu::resource_id>& group_depth, string::gpu::resource_id depth_resolve,
    bool msaa_group, bool msaa_is_first, bool msaa_is_last, VkExtent2D extent,
    const VkViewport& viewport, const VkRect2D& scissor)
{
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
    // to the depth-attachment (resolve dest) layout first; hiz.build moves it to SHADER_READ.
    // Vulkan resolve ops write in COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE (even a depth
    // resolve), so this layout transition must be made available to that stage/access — else
    // sync-validation flags a WRITE_AFTER_WRITE between the transition and the EndRendering resolve.
    // Direct barrier (not the tracker's DepthWrite scope, which would name LATE_FRAGMENT_TESTS):
    // hiz.build manages hz.depth's subsequent transitions, and it is re-discarded (UNDEFINED) here
    // every frame, so the tracker need not track it.
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
}

VkImage Renderer::image_of(string::gpu::resource_id target) const
{
    switch (target)
    {
        case string::gpu::SWAPCHAIN_TARGET: return acquired_image_;   // late-latched, stays special (M0)
        case string::gpu::COLOR_TARGET:     return allocator_.get_image(resources_.physical(color_target_)).image;
        case string::gpu::DEPTH_TARGET:     return allocator_.get_image(resources_.physical(depth_target_)).image;
        default:               return allocator_.get_image(target).image;
    }
}

VkImageView Renderer::image_view_of(string::gpu::resource_id target) const
{
    switch (target)
    {
        case string::gpu::SWAPCHAIN_TARGET: return acquired_image_view_;   // late-latched (M0)
        case string::gpu::COLOR_TARGET:     return resources_.view(color_target_);
        case string::gpu::DEPTH_TARGET:     return resources_.view(depth_target_);
        default:               return allocator_.get_image(target).view;
    }
}

std::pair<VkImageAspectFlags, uint32_t> Renderer::image_meta(string::gpu::resource_id target) const
{
    switch (target)
    {
        case string::gpu::SWAPCHAIN_TARGET: return { VK_IMAGE_ASPECT_COLOR_BIT, 1u };
        case string::gpu::COLOR_TARGET:
            return { VK_IMAGE_ASPECT_COLOR_BIT, allocator_.get_image(resources_.physical(color_target_)).mip_levels };
        case string::gpu::DEPTH_TARGET:     return { VK_IMAGE_ASPECT_DEPTH_BIT, 1u };
        default:
        {
            const string::gpu::allocated_image& img = allocator_.get_image(target);
            const bool depth = img.format == VK_FORMAT_D32_SFLOAT
                            || img.format == VK_FORMAT_D16_UNORM
                            || img.format == VK_FORMAT_D24_UNORM_S8_UINT
                            || img.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
            return { depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, img.mip_levels };
        }
    }
}

void Renderer::end_frame()
{
    auto& frame = frames_[current_frame_];

    // The swapchain image was acquired late in record_frame (just before the composite group);
    // this main submit is the FIRST submission that touches it (the async submit references no
    // swapchain state), so it waits on image-availability at the COLOR_ATTACHMENT_OUTPUT stage —
    // the first thing done to the image is the composite draw.
    // Brief 04e M4: plus any cross-lane timeline waits the async placement registered this
    // frame (each at the union of its declared read stages).
    std::vector<VkSemaphoreSubmitInfo> wait_semaphore_infos = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = acquired_wait_semaphore_,
            .value = 0,
            // ALL_COMMANDS, not COLOR_ATTACHMENT_OUTPUT. The first thing this submit does to the
            // acquired image is a LAYOUT TRANSITION, and a layout transition is a write that the
            // barrier may execute in any stage — in particular before COLOR_ATTACHMENT_OUTPUT. With
            // the narrower wait stage there is no execution dependency between the presentation
            // engine's read of that image and our transition writing it, which syncval reports as
            // SYNC-HAZARD-WRITE-AFTER-READ against vkAcquireNextImageKHR. Widening the wait is the
            // cheap half of the fix (validation's own hint (c)): the wait sits at the head of the
            // submit regardless, so nothing is serialised that was not already.
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
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

    if (const VkResult r = vkQueueSubmit2(graphics_queue_.queue, 1, &submit_info, nullptr);
        !::string::gpu::vk_report(r, "vkQueueSubmit2(graphics)"))
    {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    frame.frame_id = frame_count_;
    submissions_.mark_signaled(main_lane_, frame_count_);

    presenter_.present();

    // Single-shot capture (r.capture.frame), read LIVE.
    //
    // It used to be LATCHED at construction, which meant only the startup env could ever trigger it.
    // Reading the CVar each frame makes one mechanism serve both: the headless gates still set it
    // from the environment before this point, and a bound `system.screenshot` action or a console
    // command can now set it at runtime. Any frame index already passed — 1 is the idiom — means
    // "as soon as possible"; writing 0 back is what makes it one-shot.
    if (const int32_t at = capture_frame_cvar().get();
        at > 0 && frame_count_ >= static_cast<uint64_t>(at))
    {
        capture_color_target(capture_path_cvar().get());
        capture_frame_cvar().set(0);
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
    // Brief 07/09: encode with the SAME exposure + output-transform LUT the composite applies on
    // screen (exposure honours auto-exposure; the LUT carries grading + the aces2/aces1 curve).
    // Per-PIXEL now, not per-channel — the aces2 CAM DRT mixes channels.
    const auto encode_pixel = [&](const uint16_t* px) -> glm::vec3 {
        const glm::vec3 hdr(half_to_float(px[0]), half_to_float(px[1]), half_to_float(px[2]));
        glm::vec3 v = CompositePass::encode_display(hdr);   // display-linear [0,1]
        v = glm::pow(v, glm::vec3(1.0f / 2.2f));            // gamma
        return v;
    };
    const auto to_u8 = [](float v) -> uint8_t {
        return uint8_t(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f);
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
                const glm::vec3 v = encode_pixel(src_row + x * 4);
                row[x * 3 + 0] = to_u8(v.r);
                row[x * 3 + 1] = to_u8(v.g);
                row[x * 3 + 2] = to_u8(v.b);
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
            const glm::vec3 v = encode_pixel(src_row + x * 4);
            row[x * 3 + 0] = to_u8(v.b);  // B
            row[x * 3 + 1] = to_u8(v.g);  // G
            row[x * 3 + 2] = to_u8(v.r);  // R
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
    // Brief 09: passes that consume the resolved HDR target (the post chain) get the physical id
    // + sampled slot too. No-op for passes that don't override the hook. At init this loop is
    // empty (the passes are built after the first bind); the constructor re-notifies post-build.
    for (auto& pass : scene_passes_)
        pass->bind_color_source(slot, color_attachment_);
}

void Renderer::handle_resize(const String::View::Extent& extent)
{
    VkExtent2D vk_extent = {extent.width, extent.height};
    presenter_.resize(vk_extent);
    for (auto& pass : scene_passes_)
    {
        pass->resize(vk_extent);
    }
    // Brief 11 step 1: force a plan recompile next frame (defensive — resource ids are stable across
    // resize so the toposort is unchanged, but keep the persistent plan honest against re-created
    // targets and any pass whose usages track the extent).
    graph_dirty_ = true;

    // Release the old HDR target's bindless slot before it is destroyed, then re-allocate.
    global_descriptor_table_.unbind(color_attachment_, string::gpu::descriptor_type::TEXTURE);
    allocator_.destroy_resource(color_attachment_);
    color_attachment_ = allocator_.create_resource(string::gpu::image_info{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // Brief 09: + STORAGE (post compute writes bloom into the resolved target in place).
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    // Re-bind the new HDR target into the table and refresh the composite pass's slot (and the
    // scene passes that consume the target — the brief-09 post chain).
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

    // Brief 16 M0: re-point the `Imported` handles at the recreated targets. Ids happen to be reused
    // from the LIFO free-list today (so this is a no-op), but the registry must not depend on that —
    // this is the `recreate_viewport` intent in miniature.
    resources_.reimport(color_target_, color_attachment_);
    resources_.reimport(depth_target_, msaa_depth_);

    // The swapchain images and attachments were just recreated — their old VkImage handles
    // (and tracked layouts) are stale.
    resource_states_.clear();
}

}  // namespace String
