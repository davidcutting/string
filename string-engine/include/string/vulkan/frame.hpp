#pragma once

#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/command_recorder.hpp>

#include <volk.h>

namespace String
{

struct Frame
{
    uint64_t frame_id;

    string::gpu::deletion_queue garbage_collector;

    string::gpu::command_recorder recorder;
};

}