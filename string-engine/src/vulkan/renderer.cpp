#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <string/vulkan/driver.hpp>
#include <string/vulkan/renderer.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/vulkan/device.hpp>
#include <string/vulkan/pipelines/pipeline_2d.hpp>
#include <string/vulkan/render_data.hpp>
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

Renderer::Renderer(const ApplicationInfo& application_info, const std::shared_ptr<Window>& window)
: application_info_(application_info)
, window_(std::move(window))
, driver_(application_info, window_)
, device_(driver_, window_)
, graphics_queue_(device_.get_queue(QueueType::GRAPHICS))
, compute_queue_(device_.get_queue(QueueType::COMPUTE))
, presenter_(device_, window_, frames_in_flight_)
, allocator_({ driver_.get_instance(), device_.get_physical_device(), device_.get_device() })
, global_descriptor_table_(device_.get_device(), allocator_)
, triangle_pass_(device_, std::filesystem::path(application_info.resources_directory), static_cast<uint16_t>(frames_in_flight_))
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

    transfer_command_recorder_.init(device_.get_device(), compute_queue_);

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
    [[maybe_unused]] float delta_time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

    [[maybe_unused]] const auto swap_chain_extent = presenter_.get_extent();

    // // Resize passes
    // geometry_pass_.resize(swap_chain_extent);
    // ui_pass_.resize(swap_chain_extent);

    // // Update passes
    // geometry_pass_.update(delta_time, current_frame_);
    // ui_pass_.update(delta_time, current_frame_);
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
    acquired_wait_semaphore_ = acquired.wait_for_image_available;
    acquired_signal_semaphore_ = acquired.signal_when_ready_to_present;
}

void Renderer::begin_rendering()
{
    auto& frame = frames_[current_frame_];
    auto& command_buffer = frame.recorder.begin();
    auto& color_attachment = allocator_.get_image(color_attachment_);
    auto& depth_attachment = allocator_.get_image(depth_attachment_);

    // Transition swapchain image from UNDEFINED to COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier image_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // whatever layout its in who cares, gimme that color attach
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = color_attachment.image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }
    };

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1,
        &image_barrier);

    VkRenderingAttachmentInfo color_attachment_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = color_attachment.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {
            .color = {{ 0.0f, 0.0f, 0.0f, 0.0f }}
        }
    };

    VkImageMemoryBarrier depth_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth_attachment.image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }
    };

    vkCmdPipelineBarrier(command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1,
        &depth_barrier);

    VkRenderingAttachmentInfo depth_attachment_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depth_attachment.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {
            .depthStencil = {1.0f, 0}
        }
    };

    const auto swap_chain_extent = presenter_.get_extent();

    VkRenderingInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {{0 , 0}, swap_chain_extent},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_info,
        .pDepthAttachment = &depth_attachment_info,
        .pStencilAttachment = nullptr,
    };

    vkCmdBeginRendering(command_buffer, &rendering_info);

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(swap_chain_extent.width),
        .height = static_cast<float>(swap_chain_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = swap_chain_extent
    };
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

void Renderer::end_rendering()
{
    auto& frame = frames_[current_frame_];
    auto& command_buffer = frame.recorder.get_command_buffer();
    auto& color_attachment = allocator_.get_image(color_attachment_);

    vkCmdEndRendering(command_buffer);

    const auto extent = presenter_.get_extent();

    // Blit the offscreen HDR target into the acquired swapchain image. This is the step
    // that decouples the render target's format/resolution from the swapchain; blitting
    // into the SRGB swapchain applies the linear->sRGB encode on store. The transitions
    // bracket the blit:
    //   color_attachment_ : COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL
    //   swapchain image   : UNDEFINED -> TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR
    VkImageMemoryBarrier pre_blit_barriers[2] = {
        {   // Offscreen color target becomes the blit source.
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = color_attachment.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {   // Swapchain image becomes the blit destination.
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = acquired_image_,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(command_buffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, pre_blit_barriers);

    VkImageBlit blit_region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 } },
    };
    vkCmdBlitImage(command_buffer,
        color_attachment.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        acquired_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit_region, VK_FILTER_NEAREST);

    VkImageMemoryBarrier present_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = acquired_image_,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &present_barrier);

    // End recording only after every vkCmd* for this frame has been issued.
    frame.recorder.end();
}

void Renderer::end_frame()
{
    auto& frame = frames_[current_frame_];

    // The swapchain image was acquired in begin_frame; wait on image-availability at the
    // TRANSFER stage since the first thing we do to the swapchain image is the blit.
    const VkSemaphoreSubmitInfo wait_semaphore_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = acquired_wait_semaphore_,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
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
void Renderer::draw(Scene& scene)
{
    (void)scene;
    begin_frame();
    begin_rendering();
    triangle_pass_.record(frames_[current_frame_].recorder, static_cast<uint16_t>(current_frame_));
    end_rendering();
    end_frame();
}

void Renderer::handle_resize(const String::View::Extent& extent)
{
    VkExtent2D vk_extent = {extent.width, extent.height};
    presenter_.resize(vk_extent);

    // Clean up depth resources and re-allocate
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
}

}  // namespace String
