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
};

} // namespace string::gpu
