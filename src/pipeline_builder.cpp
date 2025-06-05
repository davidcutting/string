#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <memory>
#include <string/pipeline_builder.hpp>
#include <string/vulkan_utils.hpp>

namespace String
{

// Pipeline Layout Builder --------------------------------------------------------------

PipelineLayoutBuilder& PipelineLayoutBuilder::set_descriptor_set_layout(const std::vector<VkDescriptorSetLayout>& descriptor_set_layouts)
{
    descriptor_set_layouts_ = descriptor_set_layouts;
    return *this;
}

PipelineLayoutBuilder& PipelineLayoutBuilder::set_push_constant_ranges(const std::vector<VkPushConstantRange>& push_constant_ranges)
{
    push_constant_ranges_ = push_constant_ranges;
    return *this;
}

VkPipelineLayout PipelineLayoutBuilder::build(const std::shared_ptr<Device>& device)
{
    VkPipelineLayout pipeline_layout;

    // clang-format off
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = static_cast<uint32_t>(descriptor_set_layouts_.size()),
        .pSetLayouts = descriptor_set_layouts_.data(),
        .pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges_.size()),
        .pPushConstantRanges = push_constant_ranges_.data()
    };
    // clang-format on

    if (vkCreatePipelineLayout(device->get_device(), &pipeline_layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create pipeline layout!");
    }

    return pipeline_layout;
}

// Pipeline Builder --------------------------------------------------------------

PipelineBuilder::PipelineBuilder(const std::shared_ptr<Device>& device, const PipelineType& type)
: type_(type)
, device_(device)
{
    // Set up some defaults
    VkPipelineColorBlendAttachmentState color_blend_attachment_info = {
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = {},
        .dstColorBlendFactor = {},
        .colorBlendOp = {},
        .srcAlphaBlendFactor = {},
        .dstAlphaBlendFactor = {},
        .alphaBlendOp = {},
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                          VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT
    };
    color_blend_attachment_info_ = color_blend_attachment_info;

    VkPipelineColorBlendStateCreateInfo color_blending_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment_info_,
        .blendConstants = { 0.0f, 0.0f, 0.0f, 0.0f }
    };
    color_blending_info_ = color_blending_info;
}

PipelineBuilder& PipelineBuilder::add_vertex_shader(const std::string& resource_path)
{
    vertex_shader_module_ = vku::load_shader_from_disk(device_->get_device(), resource_path);

    // clang-format off
    VkPipelineShaderStageCreateInfo vertex_shader_stage_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vertex_shader_module_,
        .pName = "main",
        .pSpecializationInfo = nullptr
    };
    vertex_shader_stage_info_ = vertex_shader_stage_info;
    // clang-format on
    return *this;
}

PipelineBuilder& PipelineBuilder::add_fragment_shader(const std::string& resource_path)
{
    fragment_shader_module_ = vku::load_shader_from_disk(device_->get_device(), resource_path);

    // clang-format off
    VkPipelineShaderStageCreateInfo fragment_shader_stage_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fragment_shader_module_,
        .pName = "main",
        .pSpecializationInfo = nullptr
    };
    fragment_shader_stage_info_ = fragment_shader_stage_info;
    // clang-format on
    return *this;
}

PipelineBuilder& PipelineBuilder::set_vertex_binding(const VkVertexInputBindingDescription& binding_description, const std::vector<VkVertexInputAttributeDescription>& attribute_descriptions)
{
    // clang-format off
    VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding_description,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descriptions.size()),
        .pVertexAttributeDescriptions = attribute_descriptions.data()
    };
    vertex_input_info_ = vertex_input_info;
    // clang-format on
    return *this;
}

PipelineBuilder& PipelineBuilder::set_input_assembly(VkPrimitiveTopology topology, VkBool32 primitive_restart_enable)
{
    // clang-format off
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = topology,
        .primitiveRestartEnable = primitive_restart_enable
    };
    input_assembly_ = input_assembly;
    // clang-format on
    return *this;
}

PipelineBuilder& PipelineBuilder::set_tessellation(uint32_t patch_control_points)
{
    // clang-format off
    VkPipelineTessellationStateCreateInfo tessellation_state_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .patchControlPoints = patch_control_points
    };
    tessellation_state_info_ = tessellation_state_info;
    // clang-format on
    return *this;
}

PipelineBuilder& PipelineBuilder::set_rasterization(
    VkPolygonMode polygon_mode,
    VkCullModeFlags cull_mode)
{
    VkPipelineRasterizationStateCreateInfo rasterizer_state_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = polygon_mode,
        .cullMode = cull_mode,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0,
        .depthBiasClamp = 0,
        .depthBiasSlopeFactor = 0,
        .lineWidth = 1.0f
    };
    rasterizer_state_info_ = rasterizer_state_info;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_multisampling()
{
    VkPipelineMultisampleStateCreateInfo multisampling_state_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE
    };
    multisampling_state_info_ = multisampling_state_info;
    return *this;
}

PipelineBuilder& PipelineBuilder::enable_depth_stencil()
{
    VkPipelineDepthStencilStateCreateInfo depth_stencil_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {},
        .back = {},
        .minDepthBounds = 0,
        .maxDepthBounds = 0
    };
    depth_stencil_info_ = depth_stencil_info;
    return *this;
}

PipelineBuilder& PipelineBuilder::enable_color_blending()
{
    VkPipelineColorBlendAttachmentState color_blend_attachment_info = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = {},
        .srcAlphaBlendFactor = {},
        .dstAlphaBlendFactor = {},
        .alphaBlendOp = {},
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                          VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT
    };
    color_blend_attachment_info_ = color_blend_attachment_info;

    VkPipelineColorBlendStateCreateInfo color_blending_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment_info_,
        .blendConstants = { 0.0f, 0.0f, 0.0f, 0.0f }
    };
    color_blending_info_ = color_blending_info;
    return *this;
}

VkPipeline PipelineBuilder::build(const VkPipelineLayout& pipeline_layout)
{
    // Dynamic State

    // clang-format off
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr,
    };

    std::vector<VkDynamicState> dynamic_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data()
    };

    // Create the pipeline

    VkPipeline pipeline;
    VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
    VkPipelineShaderStageCreateInfo shader_stages[] = { vertex_shader_stage_info_, fragment_shader_stage_info_ };

    VkPipelineRenderingCreateInfo pipeline_render_info
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
        .stencilAttachmentFormat = {}
    };

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &pipeline_render_info,
        .flags = 0,
        .stageCount = 2,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_info_,
        .pInputAssemblyState = &input_assembly_,
        .pTessellationState = nullptr,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer_state_info_,
        .pMultisampleState = &multisampling_state_info_,
        .pDepthStencilState = &depth_stencil_info_,
        .pColorBlendState = &color_blending_info_,
        .pDynamicState = &dynamic_state,
        .layout = pipeline_layout,
        .renderPass = 0,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0
    };

    if (vkCreateGraphicsPipelines(device_->get_device(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create graphics pipeline!");
    }
    // clang-format on

    // Pipeline baked, now de-allocate shader modules
    vkDestroyShaderModule(device_->get_device(), vertex_shader_module_, nullptr);
    vkDestroyShaderModule(device_->get_device(), fragment_shader_module_, nullptr);

    return pipeline;
}

}