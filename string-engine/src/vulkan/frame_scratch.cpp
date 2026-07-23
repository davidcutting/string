#include <stdexcept>

#include <string/core/logger.hpp>
#include <string/vulkan/frame_scratch.hpp>

namespace String
{

VkDeviceSize FrameScratch::reserve(VkDeviceSize bytes, VkDeviceSize alignment)
{
    if (materialized())
        throw std::runtime_error("FrameScratch: reserve() after materialize()");
    const VkDeviceSize offset = (size_ + alignment - 1) & ~(alignment - 1);
    size_ = offset + bytes;
    return offset;
}

void FrameScratch::materialize(string::gpu::resource_allocator& allocator, uint32_t frame_slots)
{
    if (size_ == 0 || materialized()) return;
    buffers_.reserve(frame_slots);
    addresses_.reserve(frame_slots);
    for (uint32_t slot = 0; slot < frame_slots; ++slot)
    {
        const string::gpu::resource_id id = allocator.create_resource(string::gpu::buffer_info{
            .size = size_,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                   | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                   | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        buffers_.push_back(id);
        addresses_.push_back(allocator.get_buffer(id).device_address);
    }
    STRING_LOG_INFO("[scratch] {} KiB per frame slot x {} slots ({} KiB total)",
                    size_ / 1024, frame_slots, size_ * frame_slots / 1024);
}

void FrameScratch::destroy(string::gpu::resource_allocator& allocator)
{
    for (string::gpu::resource_id id : buffers_)
        allocator.destroy_resource(id);
    buffers_.clear();
    addresses_.clear();
    size_ = 0;
}

}  // namespace String
