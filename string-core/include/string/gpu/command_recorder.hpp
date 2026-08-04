#pragma once

#include <string/gpu/queue.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/resource_allocator.hpp>
#include "vulkan/vulkan_core.h"

#include <volk.h>

namespace string::gpu
{


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

    // Brief 16 (Layer 3): the recording VERBS. Thin, inline wrappers over vkCmd* on the owned
    // primary buffer — the rich recording surface passes call instead of pulling the raw handle.
    // `vk()` is the escape hatch for the paths not yet (or never) verb-migrated (transfer batch,
    // raw async, render-pass setup owned by the executor). Grows as callers adopt verbs.
    VkCommandBuffer vk() const { return primary_command_buffer_; }

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
    void begin_rendering(const VkRenderingInfo& info) { vkCmdBeginRendering(primary_command_buffer_, &info); }
    void end_rendering() { vkCmdEndRendering(primary_command_buffer_); }
};

} // namespace string::gpu
