#pragma once

#include <string/vulkan/resource.hpp>
#include <string/vulkan/pipeline.hpp>
#include <string/vulkan/command_recorder.hpp>

#include <unordered_set>

#include <volk.h>

namespace String
{

struct Pass
{
    VkDescriptorSetLayout descriptor_set_layout;
    std::vector<VkDescriptorSet> descriptor_sets;
    Pipeline pipeline;
    VkExtent2D screen_size;

    std::unordered_set<ResourceID> reads;
    std::unordered_set<ResourceID> writes;

    virtual ~Pass() = 0;
    virtual void update(const float& delta_time, const uint16_t& current_frame) = 0;
    virtual void record(CommandRecorder& recorder, const uint16_t& current_frame) = 0;
    void resize(const VkExtent2D& extent) { screen_size = extent; }
};

// A pure-virtual destructor still needs a definition so derived passes can link.
inline Pass::~Pass() = default;

} // namespace String