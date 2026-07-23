#include <stdexcept>

#include <string/gpu/submission.hpp>

namespace string::gpu
{

submission_set::~submission_set()
{
    destroy();
}

void submission_set::init(VkDevice device, const std::vector<submission_lane>& lanes,
                          uint32_t frame_slots)
{
    device_ = device;
    lanes_.clear();
    lanes_.reserve(lanes.size());
    for (const submission_lane& lane : lanes)
    {
        lane_state state;
        state.lane = lane;

        // Timeline starts at 0: the first signal (value 1+) must be strictly greater than the
        // current value (same rule the renderer's frame-pacing timeline learned the hard way).
        VkSemaphoreTypeCreateInfo timeline_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };
        const VkSemaphoreCreateInfo semaphore_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timeline_info,
            .flags = 0,
        };
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &state.timeline) != VK_SUCCESS)
            throw std::runtime_error("submission_set: failed to create lane timeline semaphore");

        state.recorders.reserve(frame_slots);
        for (uint32_t slot = 0; slot < frame_slots; ++slot)
        {
            auto rec = std::make_unique<command_recorder>();
            rec->init(device_, lane.as_queue());
            state.recorders.push_back(std::move(rec));
        }
        lanes_.push_back(std::move(state));
    }
}

void submission_set::destroy()
{
    if (device_ == VK_NULL_HANDLE) return;
    for (lane_state& state : lanes_)
    {
        for (auto& rec : state.recorders)
            if (rec) rec->destroy();
        state.recorders.clear();
        if (state.timeline != VK_NULL_HANDLE)
            vkDestroySemaphore(device_, state.timeline, nullptr);
        state.timeline = VK_NULL_HANDLE;
    }
    lanes_.clear();
    device_ = VK_NULL_HANDLE;
}

uint32_t submission_set::lane_index(std::string_view name) const
{
    for (uint32_t i = 0; i < lanes_.size(); ++i)
        if (lanes_[i].lane.name == name) return i;
    return UINT32_MAX;
}

command_recorder& submission_set::recorder(uint32_t lane_index, uint32_t frame_slot)
{
    return *lanes_[lane_index].recorders[frame_slot];
}

}  // namespace string::gpu
