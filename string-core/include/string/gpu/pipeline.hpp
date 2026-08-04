#pragma once

#include <volk.h>

namespace string::gpu
{

enum class pipeline_type
{
    GRAPHICS,
    COMPUTE
};

struct pipeline
{
    VkPushConstantRange push_constants;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    pipeline_type pipeline_type;
};

}