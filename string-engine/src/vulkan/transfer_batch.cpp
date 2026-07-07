#include <string/vulkan/transfer_batch.hpp>
#include <string/vulkan/vulkan_utils.hpp>

namespace String
{

TransferBatch::TransferBatch(string::gpu::command_recorder& recorder, string::gpu::resource_allocator& allocator)
: recorder_(recorder)
, allocator_(allocator)
{
}

VkCommandBuffer TransferBatch::begin_if_needed()
{
    if (!recording_)
    {
        recorder_.begin();
        recording_ = true;
    }
    return recorder_.get_command_buffer();
}

void TransferBatch::upload_buffer(const void* data, VkDeviceSize size, string::gpu::resource_id dst_buffer)
{
    VkCommandBuffer command_buffer = begin_if_needed();

    const string::gpu::resource_id staging = allocator_.create_staging(size);
    allocator_.copy_data_to_buffer(data, staging);
    pending_staging_.push_back(staging);

    const VkBufferCopy region = { .srcOffset = 0, .dstOffset = 0, .size = size };
    vkCmdCopyBuffer(command_buffer, allocator_.get_buffer(staging).buffer,
        allocator_.get_buffer(dst_buffer).buffer, 1, &region);
}

void TransferBatch::upload_image(const void* pixels, VkDeviceSize size, string::gpu::resource_id dst_image)
{
    VkCommandBuffer command_buffer = begin_if_needed();
    const string::gpu::allocated_image& image = allocator_.get_image(dst_image);

    vku::transition_image(command_buffer, {
        .image = image.image,
        .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .new_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .src_access = 0,
        .dst_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    });

    const string::gpu::resource_id staging = allocator_.create_staging(size);
    allocator_.copy_data_to_buffer(pixels, staging);
    pending_staging_.push_back(staging);

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

    vku::transition_image(command_buffer, {
        .image = image.image,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
    });
}

void TransferBatch::flush()
{
    if (!recording_)
    {
        return;
    }

    // Buffer copies carry no per-op barrier; make all transfer writes visible to the later
    // vertex-input / shader reads (in subsequent submissions on this queue). One global
    // barrier covers every buffer and image uploaded in this batch.
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
    vkCmdPipelineBarrier2(recorder_.get_command_buffer(), &dependency);

    // One submit + wait for the whole batch (this is a one-time load-path cost).
    recorder_.end();
    recorder_.immediate_submit();
    recorder_.reset();
    recording_ = false;

    for (const string::gpu::resource_id staging : pending_staging_)
    {
        allocator_.destroy_resource(staging);
    }
    pending_staging_.clear();
}

}  // namespace String
