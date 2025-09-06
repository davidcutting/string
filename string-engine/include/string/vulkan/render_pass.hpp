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
    Pipeline pipeline;
    std::unordered_set<ResourceID> reads;
    std::unordered_set<ResourceID> writes;

    virtual void record(const CommandRecorder& recorder) = 0;
};

} // namespace String