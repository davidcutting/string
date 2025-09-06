#pragma once

#include <memory>
#include <string/vulkan/command_recorder.hpp>

#include <volk.h>

namespace String
{

struct Frame
{
    std::unique_ptr<CommandRecorder> graphics_command_recorder;
    std::unique_ptr<CommandRecorder> compute_command_recorder;

    VkFence in_flight_fence;
    VkSemaphore image_available_semaphore;
    VkSemaphore render_complete_semaphore;

    uint32_t swapchain_image_index;
};

}