#pragma once

#include <array>
#include <cstddef>
#include <span>
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
    // The timeline value an upload will signal once the GPU has the data. Compare against
    // completed_value() / is_complete() to know when a streamed resource is safe to sample.
    // 0 is never a valid ticket (the timeline starts at 0), so it means "nothing to wait on".
    using upload_ticket = uint64_t;

    TransferBatch(string::gpu::device& device, string::gpu::resource_allocator& allocator,
                  string::gpu::queue queue);
    ~TransferBatch();

    TransferBatch(const TransferBatch&) = delete;
    TransferBatch& operator=(const TransferBatch&) = delete;

    // Records a staging copy of `data` into a device-local buffer at `dst_offset`. Nothing is
    // waited on; the batch may be submitted asynchronously once the staging budget is hit. The
    // returned ticket is the timeline value this upload's batch will signal once complete. The
    // offset lets a streamer upload a sub-range into a larger buffer (e.g. one draw's vertices).
    upload_ticket upload_buffer(const void* data, VkDeviceSize size, string::gpu::resource_id dst_buffer,
                                VkDeviceSize dst_offset = 0);
    // Records UNDEFINED -> TRANSFER_DST, a staging copy, then TRANSFER_DST -> SHADER_READ for
    // the destination image (extent taken from the allocated image). Generates the mip chain by
    // blitting when the image has more than one level — for CPU-decoded RGBA8 textures.
    upload_ticket upload_image(const void* pixels, VkDeviceSize size, string::gpu::resource_id dst_image);

    // One precomputed mip level inside the blob handed to upload_image_levels: its byte offset
    // into that blob and the level's texel extent.
    struct level_copy
    {
        VkDeviceSize offset;
        VkExtent3D extent;
    };
    // Uploads image mip levels already laid out in `data` (e.g. a transcoded KTX2 texture):
    // stages the whole blob once, copies each level verbatim into a mip, then moves the touched
    // levels to SHADER_READ. No blit — levels are taken as-is, so block-compressed formats (BC7)
    // work. Level i of `levels` targets image mip `base_mip + i`, so this both does the whole-
    // image upload (base_mip = 0, levels.size() == mip_levels) and streams a subrange of finer
    // mips into an already-live, sampled image (base_mip > 0). Only the touched subrange is
    // transitioned, so other mips stay readable.
    upload_ticket upload_image_levels(const void* data, VkDeviceSize total_size,
                                      std::span<const level_copy> levels,
                                      string::gpu::resource_id dst_image, uint32_t base_mip = 0);

    // The highest timeline value the GPU has finished (polls the timeline semaphore).
    uint64_t completed_value() const;
    // Whether the upload with this ticket has completed on the GPU. Note a ticket whose batch is
    // still only recorded (never flushed) will never complete until flush()/wait_idle() submits it.
    bool is_complete(upload_ticket ticket) const;

    // Submits any pending batch without waiting (so its GPU work can overlap what follows).
    // Called once per frame by the renderer to push streamed uploads on the graphics queue.
    void flush();
    // Submits any pending batch and blocks until every in-flight batch completes, freeing all
    // staging. The one acceptable wait: called once by the renderer before the first frame.
    void wait_idle();
};

}  // namespace String
