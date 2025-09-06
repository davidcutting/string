#pragma once

#include <volk.h>

namespace String
{

enum class PipelineType
{
    GRAPHICS,
    COMPUTE
};

struct Pipeline
{
    VkPushConstantRange push_constants;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    PipelineType pipeline_type;
};

}