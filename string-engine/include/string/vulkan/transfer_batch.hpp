#pragma once

#include <vector>

#include <string/vulkan/command_recorder.hpp>
#include <string/vulkan/resource_allocator.hpp>

#include <volk.h>

namespace String
{

// Accumulates one-time GPU uploads (buffer + image) into a single command buffer and submits
// them in one batch. Passes record their uploads during construction (via PassContext); the
// renderer flush()es once after every pass is built — one submit + one wait instead of a full
// GPU stall per copy/transition. Staging buffers are held alive until the flush completes.
class TransferBatch
{
    CommandRecorder& recorder_;
    ResourceAllocator& allocator_;
    std::vector<ResourceID> pending_staging_;
    bool recording_ = false;

    VkCommandBuffer begin_if_needed();

public:
    TransferBatch(CommandRecorder& recorder, ResourceAllocator& allocator);

    // Records a staging copy of `data` into a device-local buffer. Nothing is submitted yet.
    void upload_buffer(const void* data, VkDeviceSize size, ResourceID dst_buffer);
    // Records UNDEFINED -> TRANSFER_DST, a staging copy, then TRANSFER_DST -> SHADER_READ for
    // the destination image (extent taken from the allocated image). Nothing is submitted yet.
    void upload_image(const void* pixels, VkDeviceSize size, ResourceID dst_image);

    // Submits everything recorded so far, waits for completion, and frees staging buffers.
    // No-op if nothing was recorded (e.g. passes that don't upload).
    void flush();
};

}  // namespace String
