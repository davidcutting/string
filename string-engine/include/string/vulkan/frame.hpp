#pragma once

#include <string/gpu/resource_allocator.hpp>
#include <string/gpu/command_recorder.hpp>

#include <volk.h>

namespace String
{

// Per-frame-slot CPU-side state. Command recorders moved to the per-lane submission_set (brief
// 04e M1): acquisition is by (lane, frame slot), so a frame slot no longer owns one implicitly
// graphics-queue recorder.
struct Frame
{
    uint64_t frame_id;

    string::gpu::deletion_queue garbage_collector;
};

}