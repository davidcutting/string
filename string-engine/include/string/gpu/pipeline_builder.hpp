#pragma once

#include <string/gpu/pipeline.hpp>
#include <string/gpu/device.hpp>

#include <filesystem>
#include <string>
#include <vector>

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

    // shader stages (SPIR-V from disk — the legacy glslang path)
    pipeline_builder& add_compute_shader(const std::filesystem::path& resource_path);
    pipeline_builder& add_vertex_shader(const std::filesystem::path& resource_path);
    pipeline_builder& add_fragment_shader(const std::filesystem::path& resource_path);

    // shader stages (SPIR-V words already in memory — the in-process Slang path). `entry_point`
    // is the Slang entry-point name; Vulkan's pName must match the SPIR-V's entry point.
    pipeline_builder& add_compute_shader_spirv(const std::vector<uint32_t>& spirv, const std::string& entry_point);
    pipeline_builder& add_vertex_shader_spirv(const std::vector<uint32_t>& spirv, const std::string& entry_point);
    pipeline_builder& add_fragment_shader_spirv(const std::vector<uint32_t>& spirv, const std::string& entry_point);

    // Mesh-shader stages (VK_EXT_mesh_shader). A task (amplification) shader is optional; a mesh
    // pipeline is TASK?+MESH+FRAG?. build_mesh_pipeline() omits the vertex-input + input-assembly
    // state entirely (the mesh shader emits primitives directly).
    pipeline_builder& add_task_shader_spirv(const std::vector<uint32_t>& spirv, const std::string& entry_point);
    pipeline_builder& add_mesh_shader_spirv(const std::vector<uint32_t>& spirv, const std::string& entry_point);

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
    // without depth-testing so it doesn't occlude geometry drawn after it. compare_op defaults to
    // the engine's reverse-Z (GREATER_OR_EQUAL); pass another op for e.g. a standard-Z shadow map.
    pipeline_builder& enable_depth_stencil(bool depth_test = true, bool depth_write = true,
                                           VkCompareOp compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL);
    // Depth-only pipeline: no color attachment (colorAttachmentCount 0). For a shadow/depth prepass
    // that writes only depth. Combine with a vertex-only pipeline (no fragment shader added).
    pipeline_builder& depth_only();
    pipeline_builder& enable_color_blending();
    // Opaque single-attachment color state (blendEnable = false, RGBA writes). For passes
    // that fully cover their target and want a straight write, e.g. the composite copy.
    pipeline_builder& disable_color_blending();
    // Override the dynamic-rendering color target format (default: the offscreen HDR
    // R16G16B16A16_SFLOAT). Composite/present passes set this to the swapchain format.
    pipeline_builder& set_color_format(VkFormat format);

    VkPipeline build_graphics_pipeline(const VkPipelineLayout& pipeline_layout);
    VkPipeline build_compute_pipeline(const VkPipelineLayout& pipeline_layout);
    // Task/mesh graphics pipeline: TASK?+MESH+FRAG? stages, NO vertex-input / input-assembly state
    // (the mesh shader emits primitives). Everything else (raster/depth/blend/MSAA/formats) is the
    // same fixed state the graphics builder consumes.
    VkPipeline build_mesh_pipeline(const VkPipelineLayout& pipeline_layout);

private:
    [[maybe_unused]] pipeline_type type_;
    device& device_;
    VkShaderModule compute_shader_module_{VK_NULL_HANDLE};
    VkShaderModule vertex_shader_module_{VK_NULL_HANDLE};
    VkShaderModule fragment_shader_module_{VK_NULL_HANDLE};
    VkShaderModule task_shader_module_{VK_NULL_HANDLE};
    VkShaderModule mesh_shader_module_{VK_NULL_HANDLE};

    // Entry-point names for the SPIR-V (Slang) path. Held so the VkPipelineShaderStageCreateInfo
    // pName pointers stay valid until build(). GLSL modules default their entry point to "main".
    std::string compute_entry_point_{"main"};
    std::string vertex_entry_point_{"main"};
    std::string fragment_entry_point_{"main"};
    std::string task_entry_point_{"main"};
    std::string mesh_entry_point_{"main"};

    // Config
    VkPipelineShaderStageCreateInfo compute_shader_stage_info_{};
    VkPipelineShaderStageCreateInfo vertex_shader_stage_info_{};
    VkPipelineShaderStageCreateInfo fragment_shader_stage_info_{};
    VkPipelineShaderStageCreateInfo task_shader_stage_info_{};
    VkPipelineShaderStageCreateInfo mesh_shader_stage_info_{};
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
    bool color_enabled_ = true;   // depth_only() clears this (no color attachment)
};

}