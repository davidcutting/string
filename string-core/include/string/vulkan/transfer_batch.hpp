#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <string/gpu/device.hpp>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/resource_allocator.hpp>

#include <volk.h>

namespace string
{

// Stages one-time GPU uploads (buffer + image) and records them into a command buffer the CALLER
// owns.
//
// Brief 21 step 5: this used to own a ring of command buffers and a timeline semaphore, and submit
// itself asynchronously — a second, invisible submission path beside the frame. The renderer called
// flush() once per frame to push it. That is gone. An upload now STAGES its bytes immediately (so the
// caller may free its blob on return) and QUEUES the copy; a declared transfer pass records the queue
// into the frame's command buffer, which is what puts uploads inside the graph rather than beside it.
//
// What that buys, beyond one less submission path:
//   * the copies land at a defined point in the frame, ordered with everything else by the graph;
//   * completion is the FRAME's completion — one fence instead of a private timeline;
//   * the pre-copy barrier can be correct. The old one sourced from UNDEFINED at TOP_OF_PIPE, which
//     names no prior work at all: it neither waited for in-flight sampling of the texture being
//     overwritten (a write-after-read hazard, per-texture and timing-dependent — the streamed-texture
//     corruption class) nor preserved the mips it was not touching. Reads of a bindless texture are
//     undeclarable by construction — any pass may sample any slot — so the honest expression is one
//     conservative barrier per record: all shader reads before any transfer write.
class TransferBatch
{
public:
    // The frame index whose recording will carry an upload. `is_complete` answers once that frame has
    // retired on the GPU. 0 means "nothing to wait on".
    using upload_ticket = std::uint64_t;

    explicit TransferBatch(string::gpu::resource_allocator& allocator);
    ~TransferBatch();

    TransferBatch(const TransferBatch&) = delete;
    TransferBatch& operator=(const TransferBatch&) = delete;

    // Stage `data` and queue a copy into `dst_buffer` at `dst_offset`. The offset lets a streamer
    // upload a sub-range of a larger buffer (one draw's vertices).
    upload_ticket upload_buffer(const void* data, VkDeviceSize size, string::gpu::resource_id dst_buffer,
                                VkDeviceSize dst_offset = 0);
    // Whole-image upload from CPU-decoded pixels, generating the mip chain by blitting when the image
    // has more than one level. Construction-time path (stb textures, the 1x1 fallbacks, the UI atlas).
    upload_ticket upload_image(const void* pixels, VkDeviceSize size, string::gpu::resource_id dst_image);

    // One precomputed mip level inside the blob handed to upload_image_levels: its byte offset into
    // that blob and the level's texel extent.
    struct level_copy
    {
        VkDeviceSize offset;
        VkExtent3D extent;
    };
    // Levels already laid out in `data` (a cooked KTX2 blob): staged once, then copied verbatim into
    // mips `base_mip + i`. No blit, so block-compressed formats work. This is the streaming path —
    // it targets a subrange of an image that is already live and being sampled.
    upload_ticket upload_image_levels(const void* data, VkDeviceSize total_size,
                                      std::span<const level_copy> levels,
                                      string::gpu::resource_id dst_image, std::uint32_t base_mip = 0);

    bool is_complete(upload_ticket ticket) const { return ticket == 0 || retired_frame_ >= ticket; }
    bool pending() const { return !copies_.empty(); }

    // Record every queued copy into `rec`, with the barriers around them. Called by the declared
    // uploads pass each frame, and once by the renderer for the construction-time backlog.
    void record(gpu::command_recorder& rec);

    // The frame index uploads staged from now on belong to, and the highest frame the GPU has
    // finished. Retiring a frame frees the staging its copies used.
    void begin_frame(std::uint64_t frame, std::uint64_t retired_frame);

private:
    struct BufferCopy
    {
        string::gpu::resource_id staging;
        string::gpu::resource_id dst;
        VkDeviceSize size;
        VkDeviceSize dst_offset;
    };
    struct ImageCopy
    {
        string::gpu::resource_id staging;
        string::gpu::resource_id dst;
        std::vector<VkBufferImageCopy> regions;
        std::uint32_t base_mip;
        std::uint32_t level_count;
        bool generate_mips;          // blit the chain down from level 0 after the copy
    };
    // Staging held until the frame that recorded it retires.
    struct FrameStaging
    {
        std::uint64_t frame;
        std::vector<string::gpu::resource_id> buffers;
    };

    string::gpu::resource_id stage(const void* data, VkDeviceSize size);
    upload_ticket ticket_for_pending() const;
    // Have these mips ever been written? Decides UNDEFINED (discard, nothing to preserve) vs
    // SHADER_READ_ONLY (an already-live texture whose other content must survive).
    bool initialized(VkImage image, std::uint32_t base_mip, std::uint32_t count) const;
    void mark_initialized(VkImage image, std::uint32_t base_mip, std::uint32_t count);

    string::gpu::resource_allocator& allocator_;

    std::vector<BufferCopy> buffer_copies_;
    std::vector<ImageCopy> copies_;
    // Staging for copies not yet recorded. It is stamped with a frame at RECORD time, never at stage
    // time: uploads are staged during the app's tick, which runs BEFORE the frame begins, so stamping
    // then buckets them one frame early — and that bucket is freed while the command buffer that
    // reads it is still in flight.
    std::vector<string::gpu::resource_id> pending_staging_;
    std::vector<FrameStaging> staging_;
    // Per-image bitmask of mips already uploaded (images have far fewer than 64 levels).
    std::unordered_map<VkImage, std::uint64_t> initialized_mips_;

    std::uint64_t frame_ = 0;
    std::uint64_t retired_frame_ = 0;
    // Before the first frame, uploads are drained synchronously (flush_construction_uploads), so a
    // construction upload's ticket is 0 — "nothing to wait on" — rather than a frame that has to
    // retire first.
    bool frames_started_ = false;
};

}  // namespace string
