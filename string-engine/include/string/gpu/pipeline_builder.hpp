#pragma once

#include <string/gpu/pipeline.hpp>
#include <string/gpu/device.hpp>

#include <volk.h>

namespace string::gpu
{

class pipeline_layout_builder
{
public:
    pipeline_layout_builder& set_descriptor_set_layout(const std::vector<VkDescriptorSetLayout>& descriptor_set_layouts);
    pipeline_layout_builder& set_push_constant_ranges(const std::vector<VkPushConstantRange>& push_constant_ranges);
    VkPipelineLayout build(device& device);
private:
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts_;
    std::vector<VkPushConstantRange> push_constant_ranges_;
};

class pipeline_builder
{
public:
    explicit pipeline_builder(device& device, const pipeline_type& type = pipeline_type::GRAPHICS);
    ~pipeline_builder();

    // Non-copyable
    pipeline_builder(const pipeline_builder&) = delete;
    pipeline_builder& operator=(const pipeline_builder&) = delete;

    // shader stages
    pipeline_builder& add_compute_shader(const std::filesystem::path& resource_path);
    pipeline_builder& add_vertex_shader(const std::filesystem::path& resource_path);
    pipeline_builder& add_fragment_shader(const std::filesystem::path& resource_path);

    // pipeline config
    pipeline_builder& set_vertex_binding(
        const VkVertexInputBindingDescription& binding_description,
        const std::vector<VkVertexInputAttributeDescription>& attribute_descriptions);
    pipeline_builder& set_input_assembly(VkPrimitiveTopology topology, VkBool32 primitive_restart_enable = VK_FALSE);
    pipeline_builder& set_tessellation(uint32_t patch_control_points = 0);
    pipeline_builder& set_rasterization(
        VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL,
        VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT,
        VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE);
    pipeline_builder& set_multisampling(VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    // Declares a D32 depth attachment (matching the offscreen pass). test/write default on
    // for 3D passes; a 2D background pass (e.g. the grid) passes false to declare the format
    // without depth-testing so it doesn't occlude geometry drawn after it.
    pipeline_builder& enable_depth_stencil(bool depth_test = true, bool depth_write = true);
    pipeline_builder& enable_color_blending();
    // Opaque single-attachment color state (blendEnable = false, RGBA writes). For passes
    // that fully cover their target and want a straight write, e.g. the composite copy.
    pipeline_builder& disable_color_blending();
    // Override the dynamic-rendering color target format (default: the offscreen HDR
    // R16G16B16A16_SFLOAT). Composite/present passes set this to the swapchain format.
    pipeline_builder& set_color_format(VkFormat format);

    VkPipeline build_graphics_pipeline(const VkPipelineLayout& pipeline_layout);
    VkPipeline build_compute_pipeline(const VkPipelineLayout& pipeline_layout);

private:
    [[maybe_unused]] pipeline_type type_;
    device& device_;
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