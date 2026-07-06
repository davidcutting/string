#pragma once

#include <string/vulkan/resource_allocator.hpp>
#include <string/vulkan/command_recorder.hpp>

#include <volk.h>

namespace String
{

struct Frame
{
    uint64_t frame_id;

    DeletionQueue garbage_collector;

    CommandRecorder recorder;
};

}