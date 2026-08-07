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
#include <vector>

#include <string/core/cache_dir.hpp>
#include <string/gpu/vk_check.hpp>
#include <string/core/cvar.hpp>
#include <string/core/png_writer.hpp>
#include <string/debug_draw.hpp>
#include <string/gpu/driver.hpp>
#include <string/vulkan/renderer.hpp>
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

namespace string
{

namespace
{

core::CVar<int32_t>& capture_frame_cvar()
{
    static core::CVar<int32_t> v{
        "r.capture.frame", 0,
        "capture the presented image after this frame index then stop (0 = disabled)"};
    static const bool aliased = [] { v.add_alias("capture_frame"); return true; }();
    (void)aliased;
    return v;
}
core::CVar<std::string>& capture_path_cvar()
{
    static core::CVar<std::string> v{
        "r.capture.path", "/tmp/string_capture.bmp", "output path for r.capture.frame"};
    static const bool aliased = [] { v.add_alias("capture_path"); return true; }();
    (void)aliased;
    return v;
}
core::CVar<int32_t>& capture_every_n_cvar()
{
    static core::CVar<int32_t> v{
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

// Async-compute placement lever (alias STRING_ASYNC). Default on; off = the same graph records its
// async passes inline on the main queue — the A/B lever that isolates cross-queue sync from
// placement.
core::CVar<bool>& async_enabled_cvar()
{
    static core::CVar<bool> v{"r.async.enabled", true,
        "place dependency-free compute chains on the async compute lane (0 = record inline)"};
    static const bool aliased = [] { v.add_alias("async"); return true; }();
    (void)aliased;
    return v;
}

}  // namespace

renderer::renderer(const String::ApplicationInfo& application_info, std::shared_ptr<String::Window> window)
: application_info_(application_info)
, window_(std::move(window))
, driver_(application_info, window_)
, device_(driver_, window_)
, graphics_queue_(device_.get_queue(gpu::queue_type::GRAPHICS))
, presenter_(device_, window_, frames_in_flight_)
, allocator_({ driver_.get_instance(), device_.get_physical_device(), device_.get_device() })
, transfer_batch_(device_, allocator_, graphics_queue_)
, global_descriptor_table_(device_.get_device(), allocator_)
, shader_jobs_(1)
, file_watcher_(shader_jobs_)
// Cache lives under the per-user cache dir (the resources/shaders tree may be read-only, e.g. the
// Nix store); it is content-hash keyed so a stale entry is simply never hit. `shadercache/` beside
// the shaders is the READ-ONLY cache a package ships.
, shader_compiler_(
    core::user_cache_dir("shaders"),
    { std::filesystem::path(application_info.resources_directory) / "shaders" },
    std::filesystem::path(application_info.resources_directory) / "shadercache")
, shader_registry_(device_, shader_compiler_, file_watcher_, shader_jobs_)
, context_{
    device_,
    allocator_,
    global_descriptor_table_,
    shader_registry_,
    transfer_batch_,
    window_->get_input(),
    input_map_,
    std::filesystem::path(application_info.resources_directory),
    static_cast<uint16_t>(frames_in_flight_),
    &gpu_profiler_ctx_,
    scratch_,
}
{
    STRING_LOG_DEBUG("Initializing renderer...");

    // Per-lane command/timeline infrastructure over the device's capability-derived submission
    // lanes. The main lane's timeline IS the frame-pacing semaphore.
    submissions_.init(device_.get_device(), device_.submission_lanes(), frames_in_flight_);
    main_lane_ = submissions_.lane_index("main");
    frame_semaphore_ = submissions_.timeline(main_lane_);
    async_lane_ = submissions_.lane_index("async-compute-0");

    // Touch every capture/async CVar BEFORE apply_env so their env overrides register; the headless
    // gates set them from the environment before the first frame.
    capture_frame_cvar();
    capture_path_cvar();
    capture_every_n_cvar();
    async_enabled_cvar();
    core::CVarRegistry::instance().apply_env();

    for (auto& frame : frames_) frame.frame_id = 0;

    init_gpu_profiler();
    gpu_timing_.init(device_, frames_in_flight_);
    String::GpuProfiler::set_global(&gpu_timing_);
    String::GraphIntrospect::set_global(&introspect_);

    // NOTE: no render targets are created here. The scene's colour and depth attachments are
    // viewport-scaled TRANSIENTS the graph owns and re-sizes; the swapchain is the one image the
    // renderer still latches, because its backing is only known once the frame acquires it.
}

renderer::~renderer()
{
    vkDeviceWaitIdle(device_.get_device());
    String::GpuProfiler::set_global(nullptr);
    String::GraphIntrospect::set_global(nullptr);
#if defined(STRING_PROFILE) && !defined(STRING_RELEASE)
    for (auto ctx : lane_profiler_ctxs_)
        if (ctx) TracyVkDestroy(ctx);
#endif
    gpu_timing_.destroy(device_.get_device());
    scratch_.destroy(allocator_);
}

void renderer::wait_idle()
{
    gpu::vk_report(vkDeviceWaitIdle(device_.get_device()), "vkDeviceWaitIdle(shutdown)");
}

void renderer::publish_introspection(const compiled_frame& frame)
{
    // TODO(brief 20, step 14b): refill GraphIntrospect from the compiled graph. The debug panel's
    // snapshot type predates this brief and still describes the retired plan shape; rebuilding it
    // against compiled_frame is its own small piece of work, deliberately not faked here.
    (void)frame;
}

void renderer::begin_frame()
{
    STRING_PROFILE_SCOPE("begin_frame")
    auto& frame = frames_[current_frame_];

    const VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &frame_semaphore_,
        .pValues = &frame.frame_id,
    };
    // Bounded, not UINT64_MAX, purely so a stall is DIAGNOSABLE. A timeout means the CPU is blocked
    // on a value the GPU never signalled (our sync bug); a clean return with a frozen screen means
    // the GPU is chewing on work that never finishes (a shader hang, killed by the driver
    // watchdog). VK_EXT_device_fault reports "no fault" for both, so this is the cheapest way to
    // tell them apart on a machine we cannot attach a debugger to.
    constexpr uint64_t kFrameWaitTimeoutNs = 5'000'000'000ull;
    if (const VkResult wr = vkWaitSemaphores(device_.get_device(), &wait_info, kFrameWaitTimeoutNs);
        wr == VK_TIMEOUT)
    {
        uint64_t reached = 0;
        vkGetSemaphoreCounterValue(device_.get_device(), frame_semaphore_, &reached);
        STRING_LOG_CRITICAL("FRAME WAIT STALLED: waiting for main timeline value {} but the GPU has "
                            "only reached {} after 5s.", frame.frame_id, reached);
    }

    frame.garbage_collector.flush();

    // One acquire, one fresh semaphore, always waited, always presented. Acquiring here rather than
    // mid-frame makes it unrepresentable for a frame to present an image it never drew — the class
    // of bug where a binary semaphore belonging to a consumed frame is waited on forever.
    {
        gpu::acquired_image acquired = presenter_.acquire_next_frame();
        acquired_image_ = acquired.image;
        acquired_image_view_ = acquired.image_view;
        acquired_wait_semaphore_ = acquired.wait_for_image_available;
        acquired_signal_semaphore_ = acquired.signal_when_ready_to_present;
    }

    // Shader hot-reload at the frame boundary (this slot's GPU work has completed — see the wait
    // above). Rebuilt pipelines swap in; the old ones retire through THIS frame's garbage collector
    // so they die only after the ring cycles past all in-flight frames.
    {
        STRING_PROFILE_SCOPE("shader hot-reload poll")
        file_watcher_.poll_main_thread();
    }
    shader_registry_.apply_pending_swaps([&frame, this](gpu::pipeline old) {
        frame.garbage_collector.push_function([this, old] {
            vkDestroyPipeline(device_.get_device(), old.pipeline, nullptr);
            vkDestroyPipelineLayout(device_.get_device(), old.pipeline_layout, nullptr);
        });
    });

    // Push streamed uploads onto the graphics queue. This is the one upload path still outside the
    // graph, and only until per-frame uploads become declared transfer passes.
    {
        STRING_PROFILE_SCOPE("transfer_batch flush")
        transfer_batch_.flush();
    }
}

void renderer::render_frame(compiled_frame& frame, float dt)
{
    STRING_PROFILE_SCOPE("render_frame")
    (void)dt;

    // The frame the capture path reads. Latched here rather than handed over separately, so the two
    // can never disagree about which graph is executing.
    capture_frame_ = &frame;

    begin_frame();

    const bool async_wanted = async_enabled_cvar().get() && async_lane_ != UINT32_MAX;
    const auto slot = static_cast<std::uint32_t>(current_frame_);

    // Reset THIS slot's pool before recording into it. The slot's GPU work has completed (the
    // timeline wait in begin_frame is exactly that guarantee), so the pool is safe to recycle —
    // and without this, every frame begins a command buffer that is still in the executable state.
    gpu::command_recorder& main = main_recorder(current_frame_);
    main.reset();
    main.begin();

    gpu::command_recorder* async = nullptr;
    if (async_wanted)
    {
        async = &submissions_.recorder(async_lane_, slot);
        async->reset();
        async->begin();
    }

    gpu_timing_.begin_frame(main.vk(), slot);

    execute_info info{
        .rec = main,
        .async_rec = async,
        .frame_slot = slot,
        .extent = presenter_.get_extent(),
        .swapchain = swapchain_target,
        .swapchain_image = acquired_image_,
        .swapchain_view = acquired_image_view_,
    };
    frame.execute(info);


    async_waits_.clear();
    if (async)
    {
        async->end();
        // The async lane's timeline edge. The graph already emitted the queue-family ownership
        // halves it derived from the declarations; this is the scheduling half.
        // The async lane signals its own timeline; the main submit waits on that value. The
        // submission_set owns the timelines, so the value is simply the next one on that lane.
        const uint64_t value = submissions_.last_signaled(async_lane_) + 1;
        submit_lane(async_lane_, slot, value);
        submissions_.mark_signaled(async_lane_, value);
        async_waits_.push_back(VkSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = submissions_.timeline(async_lane_),
            .value = value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        });
    }

    main.end();
    end_frame();
}

// Submit one lane's recorder, signalling its timeline at `value`.
void renderer::submit_lane(uint32_t lane, std::uint32_t slot, uint64_t value)
{
    VkCommandBufferSubmitInfo cb{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr,
                                  submissions_.recorder(lane, slot).get_command_buffer(), 0 };
    const VkSemaphoreSubmitInfo signal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr,
        .semaphore = submissions_.timeline(lane), .value = value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .deviceIndex = 0 };
    const VkSubmitInfo2 si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .pNext = nullptr, .flags = 0,
                            .waitSemaphoreInfoCount = 0, .pWaitSemaphoreInfos = nullptr,
                            .commandBufferInfoCount = 1, .pCommandBufferInfos = &cb,
                            .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal };
    // Submit to the LANE'S queue, not the graphics queue. Its command pool was created for that
    // family, and submitting a buffer to a queue from a different family is invalid — it also
    // silently makes every layout the async work established a lie to the main lane.
    const VkQueue lane_queue = device_.submission_lanes()[lane].vk_queue;
    gpu::vk_report(vkQueueSubmit2(lane_queue, 1, &si, nullptr), "vkQueueSubmit2(async)");
}

void renderer::end_frame()
{
    auto& frame = frames_[current_frame_];

    std::vector<VkSemaphoreSubmitInfo> waits = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = acquired_wait_semaphore_,
            .value = 0,
            // ALL_COMMANDS, not COLOR_ATTACHMENT_OUTPUT. The first thing this submit does to the
            // acquired image is a LAYOUT TRANSITION, which is a write the barrier may execute in
            // any stage. A narrower wait leaves no execution dependency between the presentation
            // engine's read and our transition, which syncval reports as WRITE-AFTER-READ against
            // vkAcquireNextImageKHR. The wait sits at the head of the submit either way, so
            // widening it serialises nothing that was not already serialised.
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0
        }
    };
    waits.insert(waits.end(), async_waits_.begin(), async_waits_.end());

    const VkSemaphoreSubmitInfo signals[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = acquired_signal_semaphore_,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0
        },
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = frame_semaphore_,
            .value = frame_count_,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0
        },
    };

    VkCommandBufferSubmitInfo cb_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = main_recorder(current_frame_).get_command_buffer(),
        .deviceMask = 0
    };

    const VkSubmitInfo2 submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size()),
        .pWaitSemaphoreInfos = waits.data(),
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cb_info,
        .signalSemaphoreInfoCount = 2,
        .pSignalSemaphoreInfos = signals
    };

    if (const VkResult r = vkQueueSubmit2(graphics_queue_.queue, 1, &submit_info, nullptr);
        !gpu::vk_report(r, "vkQueueSubmit2(graphics)"))
    {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    frame.frame_id = frame_count_;
    submissions_.mark_signaled(main_lane_, frame_count_);

    presenter_.present();


    // Single-shot capture (r.capture.frame), read LIVE so both the headless env bridge and a
    // runtime console command drive the same mechanism. Writing 0 back is what makes it one-shot.
    if (const int32_t at = capture_frame_cvar().get();
        at > 0 && frame_count_ >= static_cast<uint64_t>(at))
    {
        capture(capture_path_cvar().get());
        capture_frame_cvar().set(0);
    }
    if (const int32_t every = capture_every_n_cvar().get();
        every > 0 && (frame_count_ % static_cast<uint64_t>(every)) == 0)
    {
        capture(numbered_capture_path(capture_path_cvar().get(), frame_count_));
    }

    frame_count_++;
    current_frame_ = frame_count_ % frames_in_flight_;
}

void renderer::resize(compiled_frame& frame, const String::View::Extent& extent)
{
    if (extent.width == 0 || extent.height == 0) return;

    gpu::vk_report(vkDeviceWaitIdle(device_.get_device()), "vkDeviceWaitIdle(resize)");
    presenter_.resize({ extent.width, extent.height });

    // Hand the new viewport to the graph. It re-sizes its viewport-scaled transients and forgets
    // tracked state (the backing it described no longer exists). The DECLARATIONS are untouched:
    // no re-authoring, no recompile, no plan rebuild.
    frame.resize(context_, presenter_.get_extent());
}

// Stand up the Tracy GPU contexts, one per submission lane so multi-queue overlap is visible in
// traces. Compiles to a no-op when -Dtracy is off.
//
// TracyVkContext* records, submits and waits on its probe buffer ITSELF (it calls
// vkBeginCommandBuffer internally). Our pool is created without RESET_COMMAND_BUFFER_BIT, so the
// buffer must be handed over in the INITIAL state — never begin/end it here, or Tracy's begin
// becomes an illegal implicit reset.
void renderer::init_gpu_profiler()
{
#if defined(STRING_PROFILE) && !defined(STRING_RELEASE)
    const auto& lanes = device_.submission_lanes();
    lane_profiler_ctxs_.resize(lanes.size(), nullptr);
    for (std::size_t i = 0; i < lanes.size(); ++i)
    {
        gpu::queue lane_queue = device_.get_queue(lanes[i].type);
        VkCommandBuffer probe = submissions_.recorder(static_cast<uint32_t>(i), 0).get_command_buffer();
        lane_profiler_ctxs_[i] = TracyVkContext(device_.get_physical_device(), device_.get_device(),
                                                lane_queue.queue, probe);
        TracyVkContextName(lane_profiler_ctxs_[i], lanes[i].name.c_str(),
                           static_cast<uint16_t>(lanes[i].name.size()));
    }
    gpu_profiler_ctx_ = lane_profiler_ctxs_[main_lane_];
#endif
}

// Tonemap RGBA16F to 8-bit and write a bottom-up 24-bit BMP, or a PNG when the path says so. Same
// output the gates have always compared, so existing baselines stay valid.
namespace
{
void write_capture(const std::string& path, const void* rgba16f, uint32_t w, uint32_t h)
{
    const auto* src = static_cast<const uint16_t*>(rgba16f);
    const auto half_to_float = [](uint16_t v) -> float {
        const uint32_t sign = (v >> 15) & 0x1u;
        const uint32_t exp = (v >> 10) & 0x1Fu;
        const uint32_t man = v & 0x3FFu;
        uint32_t bits;
        if (exp == 0)      bits = sign << 31;
        else if (exp == 31) bits = (sign << 31) | 0x7F800000u | (man << 13);
        else               bits = (sign << 31) | ((exp + 112u) << 23) | (man << 13);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    };

    std::vector<uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            const std::size_t si = (static_cast<std::size_t>(y) * w + x) * 4;
            const std::size_t di = (static_cast<std::size_t>(y) * w + x) * 3;
            for (int c = 0; c < 3; ++c)
            {
                const float lin = half_to_float(src[si + c]);
                const float enc = lin <= 0.0031308f ? lin * 12.92f
                                                    : 1.055f * std::pow(std::max(lin, 0.0f), 1.0f / 2.4f) - 0.055f;
                rgb[di + c] = static_cast<uint8_t>(std::clamp(enc, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }

    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0)
    {
        const std::vector<uint8_t> png = core::png::encode(rgb.data(), w, h, 3);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()),
                  static_cast<std::streamsize>(png.size()));
        return;
    }

    // 24-bit BMP, bottom-up, rows padded to 4 bytes.
    const uint32_t row = w * 3;
    const uint32_t pad = (4 - (row % 4)) % 4;
    const uint32_t image_bytes = (row + pad) * h;
    const uint32_t file_bytes = 54 + image_bytes;
    std::vector<uint8_t> header(54, 0);
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(&header[2], &file_bytes, 4);
    const uint32_t offset = 54;   std::memcpy(&header[10], &offset, 4);
    const uint32_t dib = 40;      std::memcpy(&header[14], &dib, 4);
    std::memcpy(&header[18], &w, 4);
    std::memcpy(&header[22], &h, 4);
    const uint16_t planes = 1;    std::memcpy(&header[26], &planes, 2);
    const uint16_t bpp = 24;      std::memcpy(&header[28], &bpp, 2);
    std::memcpy(&header[34], &image_bytes, 4);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    const std::vector<uint8_t> padding(pad, 0);
    for (int32_t y = static_cast<int32_t>(h) - 1; y >= 0; --y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
            const uint8_t bgr[3] = { rgb[i + 2], rgb[i + 1], rgb[i] };
            out.write(reinterpret_cast<const char*>(bgr), 3);
        }
        if (pad) out.write(reinterpret_cast<const char*>(padding.data()), pad);
    }
}
}  // namespace

// Debug capture for the headless gates: drain the GPU, copy the declared capture source to a host
// buffer, tonemap to 8-bit and write a bottom-up 24-bit BMP (or a PNG when the path says so).
void renderer::capture(const std::string& path)
{
    if (!capture_source_.valid() || capture_frame_ == nullptr)
    {
        STRING_LOG_WARN("capture requested but no capture source was declared");
        return;
    }

    gpu::vk_report(vkDeviceWaitIdle(device_.get_device()), "vkDeviceWaitIdle(capture)");

    const gpu::resource_id id = capture_frame_->physical_of(capture_source_,
                                                            static_cast<std::uint32_t>(current_frame_));
    if (id == 0) return;
    const gpu::allocated_image& img = allocator_.get_image(id);

    const VkExtent2D size{ img.extent.width, img.extent.height };
    const VkDeviceSize bytes = VkDeviceSize{ size.width } * size.height * 8;   // RGBA16F
    const gpu::resource_id staging = allocator_.create_resource(gpu::buffer_info{
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
    });

    {
        // Outside the graph on purpose: the capture drains the device first, so this is a one-shot
        // submit on its own recorder, exactly like the construction-time uploads the brief keeps out.
        // The device was drained above, so this slot's pool is idle and safe to recycle. Without the
        // reset, begin() lands on a buffer that is still in the executable state.
        gpu::command_recorder& rec = main_recorder(current_frame_);
        rec.reset();
        VkCommandBuffer cmd = rec.begin();
        String::vku::transition_image(cmd, {
            .image = img.image,
            .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .src_access = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        const VkBufferImageCopy region{
            .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { size.width, size.height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               allocator_.get_buffer(staging).buffer, 1, &region);
        String::vku::transition_image(cmd, {
            .image = img.image,
            .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_COPY_BIT,
            .src_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dst_access = VK_ACCESS_2_MEMORY_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        rec.end().immediate_submit();
    }

    write_capture(path, allocator_.get_buffer(staging).allocation_info.pMappedData,
                  size.width, size.height);
    allocator_.destroy_resource(staging);

    // The capture path transitions the source behind the graph's back, so the tracker's belief
    // about it is now stale. Telling it, rather than leaving it to guess, is the difference between
    // a debug feature and a debug feature that corrupts the next frame.
    capture_frame_->note_external_layout(capture_source_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace string
