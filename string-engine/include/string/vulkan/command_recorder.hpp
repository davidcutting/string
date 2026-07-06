#pragma once

#include <string/vulkan/queue.hpp>
#include <string/vulkan/descriptor_allocator.hpp>
#include <string/vulkan/resource_allocator.hpp>
#include "vulkan/vulkan_core.h"

#include <volk.h>

namespace String
{

struct Pass;

class CommandRecorder
{
    VkDevice device_;
    Queue queue_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer primary_command_buffer_ = VK_NULL_HANDLE;
public:
    CommandRecorder() = default;
    // No copy
    CommandRecorder(const CommandRecorder&) = delete;
    CommandRecorder& operator=(const CommandRecorder&) = delete;

    void init(const VkDevice& device, const Queue& queue);
    void destroy();

    auto begin(const VkCommandBufferUsageFlags& command_buffer_usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) -> VkCommandBuffer&;
    auto end() -> CommandRecorder&;
    auto reset() -> CommandRecorder&;

    auto get_command_buffer() -> VkCommandBuffer&;
    auto get_queue() -> Queue&;

    auto immediate_submit() -> CommandRecorder&;
};

} // namespace String
