#pragma once

#include <string/vulkan/resource_allocator.hpp>
#include <string/vulkan/descriptor_allocator_growable.hpp>
#include <string/vulkan/command_recorder.hpp>

#include <volk.h>

namespace String
{

struct Frame
{
    uint64_t frame_id;

    DescriptorAllocatorGrowable descriptor_table;
    DeletionQueue garbage_collector;

    CommandRecorder recorder;
};

}