#include <stdexcept>
#include <string/gpu/command_recorder.hpp>
#include <string/gpu/device.hpp>
#include <string/core/logger.hpp>
#include <string/gpu/queue.hpp>

namespace string::gpu
{

void command_recorder::init(VkDevice device, queue queue)
{
    device_ = device;
    queue_ = queue;

    // clang-format off
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        // .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .flags = 0,
        .queueFamilyIndex = queue.queue_family_index,
    };
    // clang-format on

    if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create command pool!");
    }

    // clang-format off
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = command_pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    // clang-format on

    if (vkAllocateCommandBuffers(device_, &alloc_info, &primary_command_buffer_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate command buffers!");
    }
}

command_recorder::~command_recorder()
{
    destroy();
}

void command_recorder::destroy()
{
    if (primary_command_buffer_ != VK_NULL_HANDLE)
    {
        vkFreeCommandBuffers(device_, command_pool_, 1, &primary_command_buffer_);
        primary_command_buffer_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
}

auto command_recorder::begin(VkCommandBufferUsageFlags command_buffer_usage) -> VkCommandBuffer&
{
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = command_buffer_usage,
        .pInheritanceInfo = nullptr,
    };

    vkBeginCommandBuffer(primary_command_buffer_, &begin_info);
    return primary_command_buffer_;
}

auto command_recorder::end() -> command_recorder&
{
    if (vkEndCommandBuffer(primary_command_buffer_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to record command buffer!");
    }
    return *this;
}

auto command_recorder::reset() -> command_recorder&
{
    if (vkResetCommandPool(device_, command_pool_, 0) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to reset command buffer!");
    }
    return *this;
}

auto command_recorder::get_command_buffer() -> VkCommandBuffer&
{
    return primary_command_buffer_;
}

auto command_recorder::get_queue() -> queue&
{
    return queue_;
}

auto command_recorder::immediate_submit() -> command_recorder&
{
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &primary_command_buffer_,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr
    };

    vkQueueSubmit(queue_.queue, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_.queue);
    return *this;
}

}