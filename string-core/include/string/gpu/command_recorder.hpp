#pragma once

#include <string/gpu/queue.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/resource_allocator.hpp>
#include "vulkan/vulkan_core.h"

#include <volk.h>

namespace string::gpu
{

// A single image layout transition, recorded with synchronization2 (VkImageMemoryBarrier2) and
// caller-supplied src/dst scopes — the barrier primitive the graph's tracker and the transfer batch
// both build on. Designated-initializer shaped, which is what makes each of its call sites a few
// lines instead of a VkImageMemoryBarrier2 + VkDependencyInfo pair.
struct image_transition
{
    VkImage image;
    VkImageLayout old_layout;
    VkImageLayout new_layout;
    VkPipelineStageFlags2 src_stage;
    VkAccessFlags2 src_access;
    VkPipelineStageFlags2 dst_stage;
    VkAccessFlags2 dst_access;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // Mip range to transition; defaults to just the base level (mip generation transitions
    // individual levels as it blits down the chain).
    uint32_t base_mip = 0;
    uint32_t level_count = 1;
    // Array layers to transition (6 for cube maps — brief 07's IBL environment). `base_layer` lets a
    // single face be transitioned on its own, which per-subresource state tracking needs: an IBL
    // cubemap's faces are written one at a time and genuinely hold different states between them.
    uint32_t base_layer = 0;
    uint32_t layer_count = 1;
};

class command_recorder
{
    VkDevice device_;
    queue queue_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer primary_command_buffer_ = VK_NULL_HANDLE;
public:
    command_recorder() = default;
    // RAII: frees the pool/buffer if still owned. Makes the recorder safe to leave to
    // stack unwinding (e.g. if a later member's constructor throws) — destroy() is also
    // callable explicitly and is idempotent.
    ~command_recorder();
    // No copy (and, with a user-declared destructor, no implicit move) — recorders are
    // owned in place as members and never relocated.
    command_recorder(const command_recorder&) = delete;
    command_recorder& operator=(const command_recorder&) = delete;

    void init(VkDevice device, queue queue);
    void destroy();

    auto begin(VkCommandBufferUsageFlags command_buffer_usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) -> VkCommandBuffer&;
    auto end() -> command_recorder&;
    auto reset() -> command_recorder&;

    auto get_command_buffer() -> VkCommandBuffer&;
    auto get_queue() -> queue&;

    auto immediate_submit() -> command_recorder&;
    // Submits the recorded buffer without waiting, signalling `signal_value` on the given
    // timeline semaphore when the GPU completes. The caller polls/waits the timeline to know
    // when the work is done (and when any staging it used can be freed). Used by the async
    // upload path so loads don't stall the CPU on vkQueueWaitIdle per batch.
    auto submit_async(VkSemaphore timeline, uint64_t signal_value) -> command_recorder&;

    // Thin, inline wrappers over vkCmd* on the owned primary buffer, so pass code names the verb
    // instead of threading the raw handle. NOT a complete surface and not intended to become one:
    // the framework below the graph (resource_state_tracker, compiled_frame::open_group/barrier_for,
    // the vku:: helpers) takes a raw `VkCommandBuffer` parameter and has no recorder to call, which
    // is why barriers and render-pass instances are recorded raw. Those paths use
    // get_command_buffer(). This block was added in 450b9ac, three days AFTER that framework was
    // written, which is why several verbs here have never had a caller.
    void dispatch(uint32_t x, uint32_t y, uint32_t z) { vkCmdDispatch(primary_command_buffer_, x, y, z); }
    void dispatch_indirect(VkBuffer buf, VkDeviceSize off) { vkCmdDispatchIndirect(primary_command_buffer_, buf, off); }
    void draw(uint32_t verts, uint32_t insts, uint32_t first_vert, uint32_t first_inst)
        { vkCmdDraw(primary_command_buffer_, verts, insts, first_vert, first_inst); }
    void draw_mesh_tasks(uint32_t x, uint32_t y, uint32_t z) { vkCmdDrawMeshTasksEXT(primary_command_buffer_, x, y, z); }
    void draw_mesh_tasks_indirect_count(VkBuffer buf, VkDeviceSize off, VkBuffer count_buf,
        VkDeviceSize count_off, uint32_t max_draws, uint32_t stride)
        { vkCmdDrawMeshTasksIndirectCountEXT(primary_command_buffer_, buf, off, count_buf, count_off, max_draws, stride); }
    void bind_pipeline(VkPipelineBindPoint bp, VkPipeline p) { vkCmdBindPipeline(primary_command_buffer_, bp, p); }
    void bind_descriptor_sets(VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
        uint32_t count, const VkDescriptorSet* sets, uint32_t dyn_count, const uint32_t* dyn)
        { vkCmdBindDescriptorSets(primary_command_buffer_, bp, layout, first, count, sets, dyn_count, dyn); }
    void bind_vertex_buffers(uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets)
        { vkCmdBindVertexBuffers(primary_command_buffer_, first, count, buffers, offsets); }
    void push_constants(VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off, uint32_t size, const void* data)
        { vkCmdPushConstants(primary_command_buffer_, layout, stages, off, size, data); }
    void set_viewport(const VkViewport& vp) { vkCmdSetViewport(primary_command_buffer_, 0, 1, &vp); }
    void set_scissor(const VkRect2D& sc) { vkCmdSetScissor(primary_command_buffer_, 0, 1, &sc); }
    void barrier(const VkDependencyInfo& dep) { vkCmdPipelineBarrier2(primary_command_buffer_, &dep); }
    void transition_image(const image_transition& t)
    {
        const VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = t.src_stage,
            .srcAccessMask = t.src_access,
            .dstStageMask = t.dst_stage,
            .dstAccessMask = t.dst_access,
            .oldLayout = t.old_layout,
            .newLayout = t.new_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = t.image,
            .subresourceRange = { t.aspect, t.base_mip, t.level_count, t.base_layer, t.layer_count },
        };
        const VkDependencyInfo dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 0,
            .pMemoryBarriers = nullptr,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };
        vkCmdPipelineBarrier2(primary_command_buffer_, &dep);
    }
    void fill_buffer(VkBuffer buf, VkDeviceSize off, VkDeviceSize size, uint32_t data)
        { vkCmdFillBuffer(primary_command_buffer_, buf, off, size, data); }
    void copy_buffer(VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy* regions)
        { vkCmdCopyBuffer(primary_command_buffer_, src, dst, count, regions); }
    void copy_buffer_to_image(VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t count, const VkBufferImageCopy* regions)
        { vkCmdCopyBufferToImage(primary_command_buffer_, src, dst, layout, count, regions); }
    void copy_image_to_buffer(VkImage src, VkImageLayout layout, VkBuffer dst, uint32_t count, const VkBufferImageCopy* regions)
        { vkCmdCopyImageToBuffer(primary_command_buffer_, src, layout, dst, count, regions); }
    void blit_image(VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
        uint32_t count, const VkImageBlit* regions, VkFilter filter)
        { vkCmdBlitImage(primary_command_buffer_, src, src_layout, dst, dst_layout, count, regions, filter); }
    void clear_color_image(VkImage img, VkImageLayout layout, const VkClearColorValue& color,
        uint32_t range_count, const VkImageSubresourceRange* ranges)
        { vkCmdClearColorImage(primary_command_buffer_, img, layout, &color, range_count, ranges); }
    void clear_depth_stencil_image(VkImage img, VkImageLayout layout, const VkClearDepthStencilValue& value,
        uint32_t range_count, const VkImageSubresourceRange* ranges)
        { vkCmdClearDepthStencilImage(primary_command_buffer_, img, layout, &value, range_count, ranges); }
    void begin_rendering(const VkRenderingInfo& info) { vkCmdBeginRendering(primary_command_buffer_, &info); }
    void end_rendering() { vkCmdEndRendering(primary_command_buffer_); }
    void reset_query_pool(VkQueryPool pool, uint32_t first, uint32_t count)
        { vkCmdResetQueryPool(primary_command_buffer_, pool, first, count); }
    void write_timestamp(VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query)
        { vkCmdWriteTimestamp2(primary_command_buffer_, stage, pool, query); }
};

} // namespace string::gpu
