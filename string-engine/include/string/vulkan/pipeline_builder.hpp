#pragma once

#include <string/vulkan/pipeline.hpp>
#include <string/vulkan/device.hpp>

#include <volk.h>

namespace String
{

class PipelineLayoutBuilder
{
public:
    PipelineLayoutBuilder& set_descriptor_set_layout(const std::vector<VkDescriptorSetLayout>& descriptor_set_layouts);
    PipelineLayoutBuilder& set_push_constant_ranges(const std::vector<VkPushConstantRange>& push_constant_ranges);
    VkPipelineLayout build(Device& device);
private:
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts_;
    std::vector<VkPushConstantRange> push_constant_ranges_;
};

class PipelineBuilder
{
public:
    explicit PipelineBuilder(Device& device, const PipelineType& type = PipelineType::GRAPHICS);
    ~PipelineBuilder();

    // Non-copyable
    PipelineBuilder(const PipelineBuilder&) = delete;
    PipelineBuilder& operator=(const PipelineBuilder&) = delete;

    // Shader stages
    PipelineBuilder& add_compute_shader(const std::filesystem::path& resource_path);
    PipelineBuilder& add_vertex_shader(const std::filesystem::path& resource_path);
    PipelineBuilder& add_fragment_shader(const std::filesystem::path& resource_path);

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
    // Declares a D32 depth attachment (matching the offscreen pass). test/write default on
    // for 3D passes; a 2D background pass (e.g. the grid) passes false to declare the format
    // without depth-testing so it doesn't occlude geometry drawn after it.
    PipelineBuilder& enable_depth_stencil(bool depth_test = true, bool depth_write = true);
    PipelineBuilder& enable_color_blending();
    // Opaque single-attachment color state (blendEnable = false, RGBA writes). For passes
    // that fully cover their target and want a straight write, e.g. the composite copy.
    PipelineBuilder& disable_color_blending();
    // Override the dynamic-rendering color target format (default: the offscreen HDR
    // R16G16B16A16_SFLOAT). Composite/present passes set this to the swapchain format.
    PipelineBuilder& set_color_format(VkFormat format);

    VkPipeline build_graphics_pipeline(const VkPipelineLayout& pipeline_layout);
    VkPipeline build_compute_pipeline(const VkPipelineLayout& pipeline_layout);

private:
    [[maybe_unused]] PipelineType type_;
    Device& device_;
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

    // Dynamic-rendering attachment formats. Depth stays UNDEFINED unless
    // enable_depth_stencil() is called, so passes without a depth attachment (e.g. the
    // composite pass) don't declare one.
    VkFormat color_format_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
};

}