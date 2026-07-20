#include <stdexcept>

#include <string/vulkan/transfer_batch.hpp>
#include <string/vulkan/vulkan_utils.hpp>

namespace String
{

TransferBatch::TransferBatch(string::gpu::device& device, string::gpu::resource_allocator& allocator,
                             string::gpu::queue queue)
: device_(device)
, allocator_(allocator)
, queue_(queue)
{
    const VkSemaphoreTypeCreateInfo timeline_type = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_type,
        .flags = 0,
    };
    if (vkCreateSemaphore(device_.get_device(), &semaphore_info, nullptr, &timeline_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create transfer timeline semaphore!");
    }

    for (Batch& batch : ring_)
    {
        batch.recorder.init(device_.get_device(), queue_);
    }
}

TransferBatch::~TransferBatch()
{
    wait_idle();
    if (timeline_ != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(device_.get_device(), timeline_, nullptr);
    }
}

void TransferBatch::wait_for(uint64_t value) const
{
    if (value == 0)
    {
        return;
    }
    const VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &timeline_,
        .pValues = &value,
    };
    vkWaitSemaphores(device_.get_device(), &wait_info, UINT64_MAX);
}

void TransferBatch::free_staging(Batch& batch)
{
    for (const string::gpu::resource_id staging : batch.staging)
    {
        allocator_.destroy_resource(staging);
    }
    batch.staging.clear();
}

VkCommandBuffer TransferBatch::begin_if_needed()
{
    Batch& batch = ring_[current_];
    if (!batch.recording)
    {
        // Reclaim this slot before reusing it: the GPU must be done with its previous submit
        // (which also means its staging is safe to free) before the pool can be reset.
        wait_for(batch.signal);
        free_staging(batch);
        if (batch.signal != 0)
        {
            batch.recorder.reset();
        }
        batch.recorder.begin();
        batch.recording = true;
        pending_bytes_ = 0;
    }
    return batch.recorder.get_command_buffer();
}

void TransferBatch::submit_current()
{
    Batch& batch = ring_[current_];
    if (!batch.recording)
    {
        return;
    }

    // Buffer copies carry no per-op barrier; make all transfer writes in this batch visible to
    // the later vertex-input / shader reads (in subsequent submissions on this queue). One
    // global barrier covers every buffer and image uploaded in the batch.
    const VkMemoryBarrier2 memory_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memory_barrier,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(batch.recorder.get_command_buffer(), &dependency);

    batch.recorder.end();
    const uint64_t signal = next_signal_++;
    batch.recorder.submit_async(timeline_, signal);
    batch.signal = signal;
    batch.recording = false;

    current_ = (current_ + 1) % RING;
}

void TransferBatch::upload_buffer(const void* data, VkDeviceSize size, string::gpu::resource_id dst_buffer)
{
    VkCommandBuffer command_buffer = begin_if_needed();

    const string::gpu::resource_id staging = allocator_.create_staging(size);
    allocator_.copy_data_to_buffer(data, staging);
    ring_[current_].staging.push_back(staging);
    pending_bytes_ += size;

    const VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = size };
    vkCmdCopyBuffer(command_buffer, allocator_.get_buffer(staging).buffer,
        allocator_.get_buffer(dst_buffer).buffer, 1, &region);

    if (pending_bytes_ >= STAGING_BUDGET)
    {
        submit_current();
    }
}

void TransferBatch::upload_image(const void* pixels, VkDeviceSize size, string::gpu::resource_id dst_image)
{
    VkCommandBuffer command_buffer = begin_if_needed();
    const string::gpu::allocated_image& image = allocator_.get_image(dst_image);
    const uint32_t mip_levels = image.mip_levels;

    // Move every mip level to TRANSFER_DST, then copy the source pixels into level 0.
    vku::transition_image(command_buffer, {
        .image = image.image,
        .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .new_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .src_access = 0,
        .dst_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .level_count = mip_levels,
    });

    const string::gpu::resource_id staging = allocator_.create_staging(size);
    allocator_.copy_data_to_buffer(pixels, staging);
    ring_[current_].staging.push_back(staging);
    pending_bytes_ += size;

    const VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { image.extent.width, image.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(command_buffer, allocator_.get_buffer(staging).buffer, image.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Generate the mip chain by successively halving with a linear blit: level i-1 (as SRC) ->
    // level i (still DST). Each source level is moved DST->SRC just before it's read.
    int32_t mip_width = static_cast<int32_t>(image.extent.width);
    int32_t mip_height = static_cast<int32_t>(image.extent.height);
    for (uint32_t level = 1; level < mip_levels; ++level)
    {
        vku::transition_image(command_buffer, {
            .image = image.image,
            .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .base_mip = level - 1,
            .level_count = 1,
        });

        const int32_t next_width = mip_width > 1 ? mip_width / 2 : 1;
        const int32_t next_height = mip_height > 1 ? mip_height / 2 : 1;
        const VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { mip_width, mip_height, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { next_width, next_height, 1 } },
        };
        vkCmdBlitImage(command_buffer,
            image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        mip_width = next_width;
        mip_height = next_height;
    }

    // Move everything to SHADER_READ: levels 0..n-2 are SRC (post-blit), level n-1 is still DST.
    if (mip_levels > 1)
    {
        vku::transition_image(command_buffer, {
            .image = image.image,
            .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .src_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
            .base_mip = 0,
            .level_count = mip_levels - 1,
        });
    }
    vku::transition_image(command_buffer, {
        .image = image.image,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
        .base_mip = mip_levels - 1,
        .level_count = 1,
    });

    if (pending_bytes_ >= STAGING_BUDGET)
    {
        submit_current();
    }
}

void TransferBatch::upload_image_levels(const void* data, VkDeviceSize total_size,
    std::span<const level_copy> levels, string::gpu::resource_id dst_image)
{
    VkCommandBuffer command_buffer = begin_if_needed();
    const string::gpu::allocated_image& image = allocator_.get_image(dst_image);
    const uint32_t mip_levels = static_cast<uint32_t>(levels.size());

    // All mip levels UNDEFINED -> TRANSFER_DST.
    vku::transition_image(command_buffer, {
        .image = image.image,
        .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .new_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .src_access = 0,
        .dst_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .level_count = mip_levels,
    });

    // Stage the whole blob once, then copy each precomputed level into its mip. Tightly packed
    // (row length / image height 0); Vulkan derives the block layout from the image's format.
    const string::gpu::resource_id staging = allocator_.create_staging(total_size);
    allocator_.copy_data_to_buffer(data, staging);
    ring_[current_].staging.push_back(staging);
    pending_bytes_ += total_size;

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mip_levels);
    for (uint32_t level = 0; level < mip_levels; ++level)
    {
        regions.push_back(VkBufferImageCopy{
            .bufferOffset = levels[level].offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = levels[level].extent,
        });
    }
    vkCmdCopyBufferToImage(command_buffer, allocator_.get_buffer(staging).buffer, image.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(regions.size()), regions.data());

    // All mip levels TRANSFER_DST -> SHADER_READ.
    vku::transition_image(command_buffer, {
        .image = image.image,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
        .level_count = mip_levels,
    });

    if (pending_bytes_ >= STAGING_BUDGET)
    {
        submit_current();
    }
}

void TransferBatch::flush()
{
    submit_current();
}

void TransferBatch::wait_idle()
{
    submit_current();
    for (Batch& batch : ring_)
    {
        wait_for(batch.signal);
        free_staging(batch);
    }
}

}  // namespace String
