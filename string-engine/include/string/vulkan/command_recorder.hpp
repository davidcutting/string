#pragma once

#include <volk.h>
#include <memory>
#include <string/vulkan/device.hpp>

namespace String
{

class CommandRecorder
{
    std::shared_ptr<Device> device_;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;

public:
    auto begin(const VkCommandBufferUsageFlags& command_buffer_usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) -> CommandRecorder&;
    auto end() -> CommandRecorder&;
    
    auto bind_pipeline() -> CommandRecorder&;
    auto bind_descriptor_sets() -> CommandRecorder&;
    auto bind_vertex_buffers() -> CommandRecorder&;
    auto bind_index_buffers() -> CommandRecorder&;

    auto draw() -> CommandRecorder&;
    auto draw_indexed() -> CommandRecorder&;

private:
    friend Device;
    explicit CommandRecorder(const std::shared_ptr<Device> device, VkCommandBuffer command_buffer);
};

}
