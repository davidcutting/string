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
    // MUST be initialized. The renderer holds these in a std::array member, which is
    // default-initialized, so without this the value is indeterminate — and begin_frame feeds it
    // straight to vkWaitSemaphores as the timeline value to wait for. A slot's first frame would
    // then wait on garbage: usually zero from fresh OS pages (harmless, which is why it hides), but
    // on a churned heap a large value stalls the frame loop for the full timeout on startup.
    uint64_t frame_id = 0;

    string::gpu::deletion_queue garbage_collector;
};

}