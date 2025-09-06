#pragma once

#include <string/vulkan/queue.hpp>
#include <string/vulkan/descriptor_allocator.hpp>
#include <string/vulkan/resource_allocator.hpp>

#include <volk.h>

namespace String
{

class Pass;

class CommandRecorder
{
    VkDevice& device_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer primary_command_buffer_ = VK_NULL_HANDLE;
    Queue queue_;

public:
    explicit CommandRecorder(VkDevice& device, const Queue& queue);
    ~CommandRecorder();

    // No copy
    CommandRecorder(const CommandRecorder&) = delete;
    CommandRecorder& operator=(const CommandRecorder&) = delete;

    auto begin(const VkCommandBufferUsageFlags& command_buffer_usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) -> CommandRecorder&;
    auto end() -> CommandRecorder&;
    auto reset() -> CommandRecorder&;

    auto get_command_buffer() -> VkCommandBuffer&;
    auto get_queue() -> Queue&;

    auto bind_descriptor_table(const DescriptorAllocator& descriptor_allocator,
                               const ResourceAllocator& resource_allocator) -> CommandRecorder&;

    auto record(const Pass& pass) -> CommandRecorder&;

    auto immediate_submit() -> CommandRecorder&;
};

} // namespace String
