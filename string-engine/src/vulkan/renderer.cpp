#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include <string/vulkan/driver.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/resource.hpp>
#include <string/vulkan/resource_allocator.hpp>
#include <string/vulkan/presenter.hpp>
#include <string/core/platform_detection.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/queue.hpp>
#include <string/vulkan/descriptor_allocator.hpp>

#include <volk.h>

namespace String
{

Renderer::Renderer(const ApplicationInfo& application_info, std::shared_ptr<Window> window,
                   const RenderPlan& plan)
: application_info_(application_info)
, window_(std::move(window))
, driver_(application_info, window_)
, device_(driver_, window_)
, graphics_queue_(device_.get_queue(QueueType::GRAPHICS))
, compute_queue_(device_.get_queue(QueueType::COMPUTE))
, presenter_(device_, window_, frames_in_flight_)
, allocator_({ driver_.get_instance(), device_.get_physical_device(), device_.get_device() })
, global_descriptor_table_(device_.get_device(), allocator_)
, composite_pass_(device_, std::filesystem::path(application_info.resources_directory), global_descriptor_table_.get_layout(), presenter_.get_format())
{
    STRING_LOG_DEBUG("Initializing renderer...");
    const auto resources_path = std::filesystem::path(application_info_.resources_directory);

    window_->register_resize_event_callback(std::bind(&Renderer::handle_resize, this, std::placeholders::_1));

    VkExtent2D extent = presenter_.get_extent();

    // Allocate render target
    color_attachment_ = allocator_.create_resource(ImageInfo{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    depth_attachment_ = allocator_.create_resource(ImageInfo{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_D32_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // Make the offscreen HDR target samplable by the composite pass via the bindless table.
    bind_composite_source();

    // Upload on the graphics queue: the texture's TRANSFER_DST -> SHADER_READ_ONLY barrier
    // uses a FRAGMENT_SHADER dst stage (only valid on a graphics-capable queue), and keeping
    // upload + sampling on one queue family avoids a queue-ownership transfer. (A dedicated
    // async-transfer queue would need explicit ownership transfers instead.)
    transfer_command_recorder_.init(device_.get_device(), graphics_queue_);

    // Build the application's declared passes now that the GPU context is ready. The plan
    // authors the content (which passes, in what order); the renderer just supplies the
    // context and executes. Passes that upload (e.g. geometry) record into the transfer batch
    // and bind into the bindless table during construction.
    TransferBatch transfer_batch{ transfer_command_recorder_, allocator_ };

    PassContext pass_context{
        device_,
        allocator_,
        global_descriptor_table_,
        transfer_batch,
        COLOR_TARGET,
        DEPTH_TARGET,
        resources_path,
        static_cast<uint16_t>(frames_in_flight_),
    };
    scene_passes_ = plan.build(pass_context);

    // Composite resolves the offscreen HDR target to the swapchain: reads color_attachment_,
    // writes the screen. Declared here (composite_pass_ is renderer-built, not in the plan).
    composite_pass_.usages = {
        { COLOR_TARGET,     Access::SampledRead, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT },
        { SWAPCHAIN_TARGET, Access::ColorWrite,  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
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

    // Submit every pass's recorded uploads in one batch and wait before the first frame.
    transfer_batch.flush();

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

    transfer_command_recorder_.destroy();

    allocator_.destroy_resource(depth_attachment_);
    allocator_.destroy_resource(color_attachment_);
}

void Renderer::update()
{
    static auto startTime = std::chrono::high_resolution_clock::now();

    auto currentTime = std::chrono::high_resolution_clock::now();
    float delta_time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

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

    update();

    // Acquire now, before recording, so end_rendering has a valid blit target. Copy the
    // handles out of AcquiredImage (which holds references into vectors resize() reallocates).
    AcquiredImage acquired = presenter_.acquire_next_frame();
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

    // The color / depth target a pass renders into (every frame pass writes exactly one color
    // target; SWAPCHAIN_TARGET means the screen).
    const auto color_target_of = [](const Pass* pass) -> ResourceID {
        for (const ResourceUsage& usage : pass->usages)
            if (usage.access == Access::ColorWrite) return usage.resource;
        return SWAPCHAIN_TARGET;
    };
    const auto depth_target_of = [](const Pass* pass) -> std::optional<ResourceID> {
        for (const ResourceUsage& usage : pass->usages)
            if (usage.access == Access::DepthWrite) return usage.resource;
        return std::nullopt;
    };

    // Group consecutive passes that share a color target and run each group as one render-pass
    // instance. All barriers are derived from the passes' declared usages via resource_states_.
    size_t start = 0;
    while (start < frame_passes_.size())
    {
        const ResourceID group_color = color_target_of(frame_passes_[start]);
        std::optional<ResourceID> group_depth;
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
        std::unordered_map<ResourceID, ResourceUsage> group_transitions;
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

        const VkRenderingAttachmentInfo color_attachment_info = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = image_view_of(group_color),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
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
            .clearValue = { .depthStencil = { 1.0f, 0 } },
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

VkImage Renderer::image_of(ResourceID target) const
{
    switch (target)
    {
        case SWAPCHAIN_TARGET: return acquired_image_;
        case COLOR_TARGET:     return allocator_.get_image(color_attachment_).image;
        case DEPTH_TARGET:     return allocator_.get_image(depth_attachment_).image;
        default:               return allocator_.get_image(target).image;
    }
}

VkImageView Renderer::image_view_of(ResourceID target) const
{
    switch (target)
    {
        case SWAPCHAIN_TARGET: return acquired_image_view_;
        case COLOR_TARGET:     return allocator_.get_image(color_attachment_).view;
        case DEPTH_TARGET:     return allocator_.get_image(depth_attachment_).view;
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

    // Increment frame
    frame_count_++;
    current_frame_ = frame_count_ % frames_in_flight_;
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
    global_descriptor_table_.bind(color_attachment_, DescriptorType::TEXTURE);
    const uint32_t slot = global_descriptor_table_.get_binding_slot(color_attachment_, DescriptorType::TEXTURE);
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
    global_descriptor_table_.unbind(color_attachment_, DescriptorType::TEXTURE);
    allocator_.destroy_resource(color_attachment_);
    color_attachment_ = allocator_.create_resource(ImageInfo{
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

    allocator_.destroy_resource(depth_attachment_);
    depth_attachment_ = allocator_.create_resource(ImageInfo{
        .extent = {extent.width, extent.height, 1},
        .format = VK_FORMAT_D32_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // The swapchain images and both attachments were just recreated — their old VkImage handles
    // (and tracked layouts) are stale.
    resource_states_.clear();
}

}  // namespace String
