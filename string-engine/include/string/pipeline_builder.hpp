#pragma once

#include <vulkan/vulkan_core.h>
#include <memory>
#include <string/device.hpp>

namespace String
{

enum class PipelineType {
    GRAPHICS,
    COMPUTE
};

class PipelineLayoutBuilder
{
public:
    PipelineLayoutBuilder& set_descriptor_set_layout(const std::vector<VkDescriptorSetLayout>& descriptor_set_layouts);
    PipelineLayoutBuilder& set_push_constant_ranges(const std::vector<VkPushConstantRange>& push_constant_ranges);
    VkPipelineLayout build(const std::shared_ptr<Device>& device);
private:
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts_;
    std::vector<VkPushConstantRange> push_constant_ranges_;
};

class PipelineBuilder
{
public:
    explicit PipelineBuilder(const std::shared_ptr<Device>& device, const PipelineType& type = PipelineType::GRAPHICS);
    ~PipelineBuilder();

    // Non-copyable, movable
    PipelineBuilder(const PipelineBuilder&) = delete;
    PipelineBuilder& operator=(const PipelineBuilder&) = delete;
    PipelineBuilder(PipelineBuilder&&) = default;
    PipelineBuilder& operator=(PipelineBuilder&&) = default;

    // Shader stages
    PipelineBuilder& add_compute_shader(const std::string& resource_path);
    PipelineBuilder& add_vertex_shader(const std::string& resource_path);
    PipelineBuilder& add_fragment_shader(const std::string& resource_path);

    // Pipeline config
    PipelineBuilder& set_vertex_binding(
        const VkVertexInputBindingDescription& binding_description,
        const std::vector<VkVertexInputAttributeDescription>& attribute_descriptions);
    PipelineBuilder& set_input_assembly(VkPrimitiveTopology topology, VkBool32 primitive_restart_enable = VK_FALSE);
    PipelineBuilder& set_tessellation(uint32_t patch_control_points = 0);
    PipelineBuilder& set_rasterization(
        VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL,
        VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT);
    PipelineBuilder& set_multisampling();
    PipelineBuilder& enable_depth_stencil();
    PipelineBuilder& enable_color_blending();

    VkPipeline build_graphics_pipeline(const VkPipelineLayout& pipeline_layout);
    VkPipeline build_compute_pipeline(const VkPipelineLayout& pipeline_layout);

private:
    PipelineType type_;
    std::shared_ptr<Device> device_;
    VkShaderModule compute_shader_module_{VK_NULL_HANDLE};
    VkShaderModule vertex_shader_module_{VK_NULL_HANDLE};
    VkShaderModule fragment_shader_module_{VK_NULL_HANDLE};

    // Config
    VkPipelineShaderStageCreateInfo compute_shader_stage_info_{};
    VkPipelineShaderStageCreateInfo vertex_shader_stage_info_{};
    VkPipelineShaderStageCreateInfo fragment_shader_stage_info_{};
    VkPipelineVertexInputStateCreateInfo vertex_input_info_{};
    VkPipelineInputAssemblyStateCreateInfo input_assembly_{};
    VkPipelineTessellationStateCreateInfo tessellation_state_info_{};
    VkPipelineRasterizationStateCreateInfo rasterizer_state_info_{};
    VkPipelineMultisampleStateCreateInfo multisampling_state_info_{};
    VkPipelineDepthStencilStateCreateInfo depth_stencil_info_{};
    VkPipelineColorBlendAttachmentState color_blend_attachment_info_{};
    VkPipelineColorBlendStateCreateInfo color_blending_info_{};
};

}