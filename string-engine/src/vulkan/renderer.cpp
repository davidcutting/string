#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

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

namespace String
{

Renderer::Renderer(const ApplicationInfo& application_info, std::shared_ptr<Window> window,
                   const RenderPlan& plan)
: application_info_(application_info)
, window_(std::move(window))
, driver_(application_info, window_)
, device_(driver_, window_)
, graphics_queue_(device_.get_queue(string::gpu::queue_type::GRAPHICS))
, compute_queue_(device_.get_queue(string::gpu::queue_type::COMPUTE))
, presenter_(device_, window_, frames_in_flight_)
, allocator_({ driver_.get_instance(), device_.get_physical_device(), device_.get_device() })
, transfer_batch_(device_, allocator_, graphics_queue_)
, global_descriptor_table_(device_.get_device(), allocator_)
// Shader hot-reload: a 1-thread pool (mtime scans + recompiles are light and serial), the watcher
// polled each frame, and the Slang compiler with a content-hash cache under the shaders dir. The
// shaders/ tree is the include/import search root.
, shader_jobs_(1)
, file_watcher_(shader_jobs_)
// Cache lives under the OS temp dir (the resources/shaders tree may be read-only, e.g. the Nix
// store); it is content-hash keyed so a stale entry is simply never hit.
, shader_compiler_(
    std::filesystem::temp_directory_path() / "string-shader-cache",
    { std::filesystem::path(application_info.resources_directory) / "shaders" })
, shader_registry_(device_, shader_compiler_, file_watcher_, shader_jobs_)
, composite_pass_(device_, std::filesystem::path(application_info.resources_directory), global_descriptor_table_.get_layout(), presenter_.get_format(), shader_registry_)
{
    STRING_LOG_DEBUG("Initializing renderer...");
    const auto resources_path = std::filesystem::path(application_info_.resources_directory);

    if (const char* cap = std::getenv("STRING_CAPTURE_FRAME"))
    {
        capture_frame_ = std::strtoull(cap, nullptr, 10);
        const char* path = std::getenv("STRING_CAPTURE_PATH");
        capture_path_ = path ? path : "/tmp/string_capture.bmp";
    }

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
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
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
    };
    scene_passes_ = plan.build(pass_context);

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
    for (const Pass* pass : frame_passes_)
        for (const ResourceUsage& usage : pass->usages)
            if (is_write(usage.access))
                written_resources_.insert(usage.resource);

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
        frame.recorder.init(device_.get_device(), graphics_queue_);
    }

    VkSemaphoreTypeCreateInfo timeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        // Start at 0: the first submit signals frame_count_ (== 1), which must be strictly
        // greater than the timeline's current value. Starting at frame_count_ (1) made the
        // first signal illegal, which broke per-frame gating (stale command-buffer reuse).
        .initialValue = 0,
    };

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_create_info,
        .flags = 0,
    };

    if (vkCreateSemaphore(device_.get_device(), &semaphore_info, nullptr, &frame_semaphore_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create timeline semaphore for resource allocator!");
    }
}

Renderer::~Renderer()
{
    STRING_LOG_DEBUG("Waiting for device to be idle...");
    vkDeviceWaitIdle(device_.get_device());

    // Destroy the passes while the allocator, table, and device are still alive.
    scene_passes_.clear();

    vkDestroySemaphore(device_.get_device(), frame_semaphore_, nullptr);

    for (auto& frame : frames_)
    {
        frame.garbage_collector.flush();
        frame.recorder.destroy();
    }

    allocator_.destroy_resource(msaa_depth_);
    allocator_.destroy_resource(msaa_color_);
    allocator_.destroy_resource(color_attachment_);
}

void Renderer::update()
{
    // Real per-frame delta (seconds since the previous update), for framerate-independent
    // motion like the camera. First frame clamps to ~0.
    static auto last_time = std::chrono::high_resolution_clock::now();
    const auto current_time = std::chrono::high_resolution_clock::now();
    const float delta_time =
        std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_time).count();
    last_time = current_time;

    // Rolling frame-time log (until Tracy is wired): avg/max ms per 600 frames.
    static float acc = 0.0f, worst = 0.0f;
    static uint32_t n = 0;
    acc += delta_time; worst = std::max(worst, delta_time); ++n;
    if (n == 600)
    {
        STRING_LOG_INFO("[frametime] avg {:.2f} ms ({:.0f} fps), worst {:.2f} ms",
                        acc / n * 1000.0f, n / acc, worst * 1000.0f);
        acc = 0.0f; worst = 0.0f; n = 0;
    }

    for (auto& pass : scene_passes_)
    {
        pass->update(delta_time, static_cast<uint16_t>(current_frame_));
    }
}

void Renderer::begin_frame()
{
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
    frame.recorder.reset();

    // Shader hot-reload, at the frame boundary (GPU work for this slot has completed — see the
    // semaphore wait above). Poll the file watcher for edits, then apply any completed recompiles:
    // swap the rebuilt pipeline in and retire the old one through THIS frame's garbage collector,
    // so it is destroyed only after the ring cycles back (past all in-flight frames).
    file_watcher_.poll_main_thread();
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
    transfer_batch_.flush();

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
    auto& frame = frames_[current_frame_];
    VkCommandBuffer command_buffer = frame.recorder.begin();
    const VkExtent2D extent = presenter_.get_extent();

    const VkViewport viewport = {
        .x = 0.0f, .y = 0.0f,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    const VkRect2D scissor = { .offset = { 0, 0 }, .extent = extent };

    // Compute prepass: passes may dispatch GPU work (e.g. frustum culling that fills an indirect
    // buffer) outside dynamic rendering, before any graphics group. If any did, one barrier makes
    // those storage writes visible to the indirect draws / vertex-stage reads that follow.
    bool recorded_compute = false;
    for (Pass* pass : frame_passes_)
        recorded_compute |= pass->record_compute(frame.recorder, static_cast<uint16_t>(current_frame_));
    if (recorded_compute)
    {
        const VkMemoryBarrier2 compute_to_draw = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            // Compute output feeds indirect draws, vertex pulling, AND fragment reads (froxel light
            // lists from the Forward+ binning compute are consumed in the fragment stage).
            .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                          | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        };
        const VkDependencyInfo dependency = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &compute_to_draw,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 0,
            .pImageMemoryBarriers = nullptr,
        };
        vkCmdPipelineBarrier2(command_buffer, &dependency);
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
        return std::nullopt;
    };

    // Group consecutive passes that share a color target and run each group as one render-pass
    // instance. All barriers are derived from the passes' declared usages via resource_states_.
    size_t start = 0;
    while (start < frame_passes_.size())
    {
        const string::gpu::resource_id group_color = color_target_of(frame_passes_[start]);
        std::optional<string::gpu::resource_id> group_depth;
        size_t end = start;
        while (end < frame_passes_.size() && color_target_of(frame_passes_[end]) == group_color)
        {
            if (auto depth = depth_target_of(frame_passes_[end])) group_depth = depth;
            ++end;
        }

        // Barriers: transition each graph-written image the group touches to the state its usage
        // needs (deduped per resource). Static uploaded inputs are skipped — they aren't in
        // written_resources_, and buffer usages map to VK_IMAGE_LAYOUT_UNDEFINED. Reads of a
        // graph-written resource (the composite sampling color) get the correct source layout
        // from the tracker; writes discard the previous contents.
        std::unordered_map<string::gpu::resource_id, ResourceUsage> group_transitions;
        for (size_t i = start; i < end; ++i)
            for (const ResourceUsage& usage : frame_passes_[i]->usages)
            {
                if (!written_resources_.contains(usage.resource)) continue;
                if (access_scope(usage.access).layout == VK_IMAGE_LAYOUT_UNDEFINED) continue;
                group_transitions[usage.resource] = usage;
            }
        for (const auto& [resource, usage] : group_transitions)
        {
            const VkImageAspectFlags aspect =
                (usage.access == Access::DepthWrite || usage.access == Access::DepthRead)
                    ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            resource_states_.transition(command_buffer, image_of(resource), aspect,
                usage.access, usage.stage, /*discard=*/is_write(usage.access));
        }

        // The scene group (COLOR_TARGET) is multisampled: passes render into msaa_color_ and it's
        // resolved into color_attachment_ (which the group_transitions loop already moved to
        // COLOR_ATTACHMENT_OPTIMAL as the resolve dest). The composite group (SWAPCHAIN) is
        // single-sample and renders straight into the swapchain image.
        const bool msaa_group = (group_color == string::gpu::COLOR_TARGET);
        if (msaa_group)
        {
            // msaa_color_ isn't a graph resource; transition it here (discard — fully cleared).
            resource_states_.transition(command_buffer, allocator_.get_image(msaa_color_).image,
                VK_IMAGE_ASPECT_COLOR_BIT, Access::ColorWrite,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, /*discard=*/true);
        }

        const VkRenderingAttachmentInfo color_attachment_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = msaa_group ? allocator_.get_image(msaa_color_).view : image_view_of(group_color),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            // Resolve the multisampled scene color down into color_attachment_ (image_view_of the
            // group's COLOR_TARGET) at end of rendering; the MS samples themselves aren't kept.
            .resolveMode = msaa_group ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
            .resolveImageView = msaa_group ? image_view_of(group_color) : VK_NULL_HANDLE,
            .resolveImageLayout = msaa_group ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = msaa_group ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = {{ 0.0f, 0.0f, 0.0f, 0.0f }} },
        };
        const VkRenderingAttachmentInfo depth_attachment_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = group_depth ? image_view_of(*group_depth) : VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
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

        for (size_t i = start; i < end; ++i)
            frame_passes_[i]->record(frame.recorder, static_cast<uint16_t>(current_frame_));

        vkCmdEndRendering(command_buffer);
        start = end;
    }

    // The swapchain was rendered by the final group; ready it for presentation.
    resource_states_.transition(command_buffer, acquired_image_, VK_IMAGE_ASPECT_COLOR_BIT,
        Access::Present, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

    frame.recorder.end();
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
    const VkSemaphoreSubmitInfo wait_semaphore_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = acquired_wait_semaphore_,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .deviceIndex = 0
        }
    };

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
        .commandBuffer = frame.recorder.get_command_buffer(),
        .deviceMask = 0
    };

    VkSubmitInfo2 submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = wait_semaphore_infos,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_buffer_info,
        .signalSemaphoreInfoCount = 2,
        .pSignalSemaphoreInfos = signal_semaphore_infos
    };

    if (vkQueueSubmit2(frame.recorder.get_queue().queue, 1, &submit_info, nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    frame.frame_id = frame_count_;

    presenter_.present();

    if (capture_frame_ != 0 && frame_count_ >= capture_frame_)
    {
        capture_color_target();
        capture_frame_ = 0;
    }

    // Increment frame
    frame_count_++;
    current_frame_ = frame_count_ % frames_in_flight_;
}

// Debug capture: drain the GPU, copy color_attachment_ (single-sample resolved HDR) to a host
// buffer, tonemap to 8-bit, write a bottom-up 24-bit BMP. Transitions go through
// resource_states_ so the tracker stays consistent for the next frame.
void Renderer::capture_color_target()
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
    std::ofstream out(capture_path_, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bmp.data()), std::streamsize(bmp.size()));
    out.close();
    allocator_.destroy_resource(staging);
    STRING_LOG_INFO("[capture] frame {} -> {}", frame_count_, capture_path_);
}

// The whole-frame entry point, called once per frame from Application::run.
void Renderer::draw()
{
    begin_frame();
    record_frame();
    end_frame();
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
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
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
