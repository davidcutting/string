#include <algorithm>
#include <stdexcept>

#include <string/vulkan/transfer_batch.hpp>
#include <string/vulkan/vulkan_utils.hpp>
#include <string/core/profiler.hpp>

namespace string
{

TransferBatch::TransferBatch(string::gpu::resource_allocator& allocator)
: allocator_(allocator)
{
}

TransferBatch::~TransferBatch()
{
    // Everything still held is either recorded into a command buffer the owner has already waited
    // out (it destroys the device after us) or never recorded at all. Either way the staging is ours
    // to free.
    for (const FrameStaging& fs : staging_)
        for (const string::gpu::resource_id id : fs.buffers) allocator_.destroy_resource(id);
    for (const string::gpu::resource_id id : pending_staging_) allocator_.destroy_resource(id);
}

void TransferBatch::begin_frame(std::uint64_t frame, std::uint64_t retired_frame)
{
    frame_ = frame;
    retired_frame_ = retired_frame;
    frames_started_ = true;
    // Free the staging of every frame the GPU has finished with. This is the whole lifetime rule:
    // staging outlives the record, not the submit.
    std::erase_if(staging_, [&](const FrameStaging& fs) {
        if (fs.frame > retired_frame_) return false;
        for (const string::gpu::resource_id id : fs.buffers) allocator_.destroy_resource(id);
        return true;
    });
}

string::gpu::resource_id TransferBatch::stage(const void* data, VkDeviceSize size)
{
    const string::gpu::resource_id staging = allocator_.create_staging(size);
    allocator_.copy_data_to_buffer(data, staging);
    pending_staging_.push_back(staging);
    return staging;
}

// The earliest frame that can carry this upload. Staging happens during the app's tick, before the
// frame begins, so the recording frame is the NEXT one — a ticket of `frame_` would report complete
// while the copy was still only staged.
TransferBatch::upload_ticket TransferBatch::ticket_for_pending() const
{
    return frames_started_ ? frame_ + 1 : 0;
}

bool TransferBatch::initialized(VkImage image, std::uint32_t base_mip, std::uint32_t count) const
{
    const auto it = initialized_mips_.find(image);
    if (it == initialized_mips_.end()) return false;
    const std::uint64_t want = count >= 64 ? ~std::uint64_t{ 0 }
                                           : (((std::uint64_t{ 1 } << count) - 1) << base_mip);
    return (it->second & want) != 0;
}

void TransferBatch::mark_initialized(VkImage image, std::uint32_t base_mip, std::uint32_t count)
{
    const std::uint64_t bits = count >= 64 ? ~std::uint64_t{ 0 }
                                           : (((std::uint64_t{ 1 } << count) - 1) << base_mip);
    initialized_mips_[image] |= bits;
}

TransferBatch::upload_ticket TransferBatch::upload_buffer(const void* data, VkDeviceSize size,
                                                          string::gpu::resource_id dst_buffer,
                                                          VkDeviceSize dst_offset)
{
    buffer_copies_.push_back({ stage(data, size), dst_buffer, size, dst_offset });
    return ticket_for_pending();
}

TransferBatch::upload_ticket TransferBatch::upload_image(const void* pixels, VkDeviceSize size,
                                                         string::gpu::resource_id dst_image)
{
    STRING_PROFILE_SCOPE("upload image")
    const string::gpu::allocated_image& image = allocator_.get_image(dst_image);
    ImageCopy copy{};
    copy.staging = stage(pixels, size);
    copy.dst = dst_image;
    copy.base_mip = 0;
    copy.level_count = image.mip_levels;
    copy.generate_mips = image.mip_levels > 1;
    copy.regions.push_back(VkBufferImageCopy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { image.extent.width, image.extent.height, 1 },
    });
    copies_.push_back(std::move(copy));
    return ticket_for_pending();
}

TransferBatch::upload_ticket TransferBatch::upload_image_levels(
    const void* data, VkDeviceSize total_size, std::span<const level_copy> levels,
    string::gpu::resource_id dst_image, std::uint32_t base_mip)
{
    ImageCopy copy{};
    copy.staging = stage(data, total_size);
    copy.dst = dst_image;
    copy.base_mip = base_mip;
    copy.level_count = static_cast<std::uint32_t>(levels.size());
    copy.generate_mips = false;
    copy.regions.reserve(levels.size());
    for (std::uint32_t level = 0; level < levels.size(); ++level)
    {
        copy.regions.push_back(VkBufferImageCopy{
            .bufferOffset = levels[level].offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, base_mip + level, 0, 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = levels[level].extent,
        });
    }
    copies_.push_back(std::move(copy));
    return ticket_for_pending();
}

void TransferBatch::record(gpu::command_recorder& rec)
{
    if (buffer_copies_.empty() && copies_.empty()) return;
    STRING_PROFILE_SCOPE("transfer record")

    // ONE conservative barrier ahead of every copy in this batch, and it is the fix for the defect
    // the old per-upload barriers had: writing over a texture that in-flight work may still be
    // SAMPLING. Bindless reads cannot be declared — any pass may index any slot — so nothing derives
    // this edge and it has to be stated. ALL_COMMANDS/SHADER_READ is the honest scope; transfers are
    // rare (a streamed mip, a newly resident draw's vertices), so its cost is not measurable.
    const VkMemoryBarrier2 war = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    };
    const VkDependencyInfo war_dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                       .memoryBarrierCount = 1, .pMemoryBarriers = &war };
    rec.barrier(war_dep);

    for (const BufferCopy& bc : buffer_copies_)
    {
        const VkBufferCopy region{ .srcOffset = 0, .dstOffset = bc.dst_offset, .size = bc.size };
        rec.copy_buffer(allocator_.get_buffer(bc.staging).buffer,
                        allocator_.get_buffer(bc.dst).buffer, 1, &region);
    }

    for (const ImageCopy& ic : copies_)
    {
        const string::gpu::allocated_image& image = allocator_.get_image(ic.dst);
        // Only the touched subrange changes layout, so the mips this upload is NOT replacing stay
        // readable. Sourcing from UNDEFINED discards, which is right ONLY the first time these levels
        // are written; an already-live texture keeps its layout so its contents survive.
        const bool live = initialized(image.image, ic.base_mip, ic.level_count);
        rec.transition_image({
            .image = image.image,
            .old_layout = live ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .new_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .src_access = 0,
            .dst_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .base_mip = ic.base_mip,
            .level_count = ic.level_count,
        });

        rec.copy_buffer_to_image(allocator_.get_buffer(ic.staging).buffer, image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(ic.regions.size()), ic.regions.data());

        if (ic.generate_mips)
        {
            // Halve down the chain with a linear blit: level i-1 (as SRC) into level i (still DST).
            std::int32_t w = static_cast<std::int32_t>(image.extent.width);
            std::int32_t h = static_cast<std::int32_t>(image.extent.height);
            for (std::uint32_t level = 1; level < image.mip_levels; ++level)
            {
                rec.transition_image({
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
                const std::int32_t nw = w > 1 ? w / 2 : 1;
                const std::int32_t nh = h > 1 ? h / 2 : 1;
                const VkImageBlit blit = {
                    .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 },
                    .srcOffsets = { { 0, 0, 0 }, { w, h, 1 } },
                    .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                    .dstOffsets = { { 0, 0, 0 }, { nw, nh, 1 } },
                };
                rec.blit_image(image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
                w = nw;
                h = nh;
            }
            // Levels 0..n-2 are SRC after the blits; the last is still DST.
            if (image.mip_levels > 1)
            {
                rec.transition_image({
                    .image = image.image,
                    .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    .src_access = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                    .base_mip = 0,
                    .level_count = image.mip_levels - 1,
                });
            }
            rec.transition_image({
                .image = image.image,
                .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                .base_mip = image.mip_levels - 1,
                .level_count = 1,
            });
            mark_initialized(image.image, 0, image.mip_levels);
        }
        else
        {
            // ALL_COMMANDS, not FRAGMENT_SHADER: the meshlet TASK shaders sample bindless textures
            // too, and a destination scope that names only the fragment stage does not cover them.
            rec.transition_image({
                .image = image.image,
                .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .src_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
                .base_mip = ic.base_mip,
                .level_count = ic.level_count,
            });
            mark_initialized(image.image, ic.base_mip, ic.level_count);
        }
    }

    // Buffer copies carry no per-op barrier; one global barrier makes every transfer write in this
    // batch visible to the reads that follow it in this command buffer.
    const VkMemoryBarrier2 visible = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
    };
    const VkDependencyInfo visible_dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                           .memoryBarrierCount = 1, .pMemoryBarriers = &visible };
    rec.barrier(visible_dep);

    buffer_copies_.clear();
    copies_.clear();
    // The staging these copies read is now owned by THIS frame's command buffer, and only that frame
    // retiring makes it free-able. Stamping here rather than at stage time is the whole rule.
    if (!pending_staging_.empty()) staging_.push_back({ frame_, std::move(pending_staging_) });
    pending_staging_.clear();
}

}  // namespace string
