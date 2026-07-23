#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/queue.hpp>

#include <volk.h>

namespace string::gpu
{

// Brief 04e M1: per-lane command + timeline infrastructure. One instance owns, for EVERY
// submission lane the device exposes:
//   - a command recorder (its own pool) per frame slot, so recorder acquisition is tied to
//     (lane, frame) and pools never cross queues or in-flight frames, and
//   - one timeline semaphore, centrally managed, for frame pacing and cross-lane edges (the
//     graph scheduler signals/waits these; nothing else creates ad-hoc timelines for queue work).
//
// Multithreaded recording is explicitly OUT of scope (one recorder per (lane, frame) is the
// seam a future job-recorded frame would widen into N recorders per slot).
class submission_set
{
public:
    submission_set() = default;
    ~submission_set();
    submission_set(const submission_set&) = delete;
    submission_set& operator=(const submission_set&) = delete;

    void init(VkDevice device, const std::vector<submission_lane>& lanes, uint32_t frame_slots);
    void destroy();

    uint32_t lane_count() const { return static_cast<uint32_t>(lanes_.size()); }
    const submission_lane& lane(uint32_t lane_index) const { return lanes_[lane_index].lane; }
    // Index of a named lane; UINT32_MAX when the hardware doesn't expose it (capability table
    // said so) — callers degrade explicitly, never silently multiplex.
    uint32_t lane_index(std::string_view name) const;

    // The recorder for (lane, frame slot). Reset/record/submit discipline is the caller's
    // (the renderer's frame loop): one begin/end cycle per slot per frame.
    command_recorder& recorder(uint32_t lane_index, uint32_t frame_slot);

    // The lane's timeline semaphore. Values are managed by the caller through
    // mark_signaled/last_signaled so cross-lane waits can be expressed against the latest
    // submitted value without re-deriving it.
    VkSemaphore timeline(uint32_t lane_index) const { return lanes_[lane_index].timeline; }
    uint64_t last_signaled(uint32_t lane_index) const { return lanes_[lane_index].signaled; }
    void mark_signaled(uint32_t lane_index, uint64_t value) { lanes_[lane_index].signaled = value; }

private:
    struct lane_state
    {
        submission_lane lane;
        VkSemaphore timeline = VK_NULL_HANDLE;
        uint64_t signaled = 0;
        // unique_ptr because command_recorder is intentionally non-movable (owned in place).
        std::vector<std::unique_ptr<command_recorder>> recorders;  // [frame_slot]
    };

    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<lane_state> lanes_;
};

}  // namespace string::gpu
