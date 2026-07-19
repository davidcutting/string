#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/queue.hpp>
#include <string/gpu/resource_allocator.hpp>

#include <volk.h>

namespace String
{

// Streams one-time GPU uploads (buffer + image) to the device without stalling the CPU.
//
// Uploads are recorded into a small ring of command buffers and submitted asynchronously,
// each signalling a monotonically increasing value on an owned timeline semaphore. Staging
// buffers are held until their submit's timeline value is reached, then freed. When the
// recorded staging exceeds a byte budget the current batch is submitted and the next ring
// slot is taken — reusing a slot waits for its previous submit, which bounds how much staging
// is resident at once (the guard the old per-texture flush provided) while still overlapping
// the GPU copies with CPU decode/record (which the old per-texture vkQueueWaitIdle did not).
//
// Uploads run on the graphics queue: the image TRANSFER_DST -> SHADER_READ barrier uses a
// FRAGMENT_SHADER destination stage (graphics-only), and sharing the queue family with the
// later sampling avoids a queue-ownership transfer. Passes record their uploads during
// construction (via PassContext); the renderer drains once with wait_idle() before the first
// frame.
class TransferBatch
{
    // A ring slot: its own command recorder plus the staging it must keep alive until the GPU
    // reaches `signal` on the timeline. `signal == 0` means the slot has never been submitted.
    struct Batch
    {
        string::gpu::command_recorder recorder;
        std::vector<string::gpu::resource_id> staging;
        uint64_t signal = 0;
        bool recording = false;
    };

    // Two slots overlap CPU record of one batch with GPU execution of the other; the byte
    // budget caps resident staging at roughly STAGING_BUDGET * RING.
    static constexpr std::size_t RING = 2;
    static constexpr VkDeviceSize STAGING_BUDGET = 256ull * 1024 * 1024;

    string::gpu::device& device_;
    string::gpu::resource_allocator& allocator_;
    string::gpu::queue queue_;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t next_signal_ = 1;

    std::array<Batch, RING> ring_;
    std::size_t current_ = 0;
    VkDeviceSize pending_bytes_ = 0;

    // Ensures the current slot is recording, reclaiming it first (wait its prior submit, free
    // its staging, reset the pool) if it isn't. Returns the command buffer to record into.
    VkCommandBuffer begin_if_needed();
    // Ends + async-submits the current slot (with a trailing global barrier), then advances the
    // ring. No-op if the current slot isn't recording.
    void submit_current();
    void wait_for(uint64_t value) const;
    void free_staging(Batch& batch);

public:
    TransferBatch(string::gpu::device& device, string::gpu::resource_allocator& allocator,
                  string::gpu::queue queue);
    ~TransferBatch();

    TransferBatch(const TransferBatch&) = delete;
    TransferBatch& operator=(const TransferBatch&) = delete;

    // Records a staging copy of `data` into a device-local buffer. Nothing is waited on; the
    // batch may be submitted asynchronously once the staging budget is hit.
    void upload_buffer(const void* data, VkDeviceSize size, string::gpu::resource_id dst_buffer);
    // Records UNDEFINED -> TRANSFER_DST, a staging copy, then TRANSFER_DST -> SHADER_READ for
    // the destination image (extent taken from the allocated image).
    void upload_image(const void* pixels, VkDeviceSize size, string::gpu::resource_id dst_image);

    // Submits any pending batch without waiting (so its GPU work can overlap what follows).
    void flush();
    // Submits any pending batch and blocks until every in-flight batch completes, freeing all
    // staging. The one acceptable wait: called once by the renderer before the first frame.
    void wait_idle();
};

}  // namespace String
