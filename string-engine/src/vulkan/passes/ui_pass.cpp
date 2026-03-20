#include <cstdint>
#include <filesystem>
#include <cstring>

#include <span>
#include <string/vulkan/passes/ui_pass.hpp>

namespace String
{

UIPass::UIPass(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight)
: device_(device)
{
    // Descriptor set layouts
    // clang-format off
    VkDescriptorSetLayoutBinding ubo_layout_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr
    };

    VkDescriptorSetLayoutBinding ssbo_layout_binding = {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr
    };
    // clang-format on

    std::vector<VkDescriptorSetLayoutBinding> bindings = {ubo_layout_binding, ssbo_layout_binding};

    // clang-format off
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };
    // clang-format on

    if (vkCreateDescriptorSetLayout(device_.get_device(), &layout_info, nullptr, &descriptor_set_layout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create descriptor set layout!");
    }

    // Note: In tutorial code the construction of descriptor set layouts and the actual descriptor sets were seperated

    // Descriptor Sets
    std::vector<VkDescriptorSetLayout> ui_layouts(frames_in_flight, descriptor_set_layout);
    VkDescriptorSetAllocateInfo ui_descriptor_set_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = static_cast<uint32_t>(frames_in_flight),
        .pSetLayouts = ui_layouts.data()
    };
    descriptor_sets.resize(frames_in_flight);

    if (vkAllocateDescriptorSets(device_.get_device(), &ui_descriptor_set_alloc_info, descriptor_sets.data()) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate UI descriptor sets!");
    }

    for (size_t i = 0; i < frames_in_flight; i++) {
        VkDescriptorBufferInfo ui_shapes_ssbo_info = {
            .buffer = ui_shapes_ssbo_[i]->buffer,
            .offset = 0,
            .range = ui_shapes_ssbo_[i]->buffer_size
        };

        std::vector<VkWriteDescriptorSet> descriptor_writes = {
            // {
            //     .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            //     .pNext = nullptr,
            //     .dstSet = ui_descriptor_sets_[i],
            //     .dstBinding = 0,
            //     .dstArrayElement = 0,
            //     .descriptorCount = 1,
            //     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            //     .pImageInfo = nullptr,
            //     .pBufferInfo = &camera_2d_ubo_info,
            //     .pTexelBufferView = nullptr,
            // },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = descriptor_sets[i],
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &ui_shapes_ssbo_info,
                .pTexelBufferView = nullptr,
            }
        };

        vkUpdateDescriptorSets(
            device_.get_device(),
            static_cast<uint32_t>(descriptor_writes.size()),
            descriptor_writes.data(), 0,
            nullptr);
    }

    // Pipelines
    pipeline.push_constants = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(UIShaderConfig)
    };

    pipeline_2d_ = std::make_unique<Pipeline2D>(
        resources_path,
        device,
        descriptor_set_layout,
        pipeline.push_constants
    );

    // Resources
    ui_shapes_ssbo_.resize(frames_in_flight);
    ui_elements_mapped_.resize(frames_in_flight);

    uint32_t num_elements = 10;
    VkDeviceSize shapes_buffer_size = sizeof(UIElement) * num_elements;
    void* shapes_buffer_mapping;

    for (size_t i = 0; i < frames_in_flight; i++)
    {
        ui_shapes_ssbo_[i] = device_.get_allocator().create_buffer(shapes_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

        vmaMapMemory(
            device_.get_allocator().get_allocator(),
            ui_shapes_ssbo_[i]->allocation,
            &shapes_buffer_mapping
        );

        ui_elements_mapped_[i] = std::span<UIElement>(
            reinterpret_cast<UIElement*>(shapes_buffer_mapping),
            num_elements
        );
    }
}

UIPass::~UIPass()
{
    pipeline_2d_.reset();

    for (auto& ssbo : ui_shapes_ssbo_)
    {
        vmaUnmapMemory(device_.get_allocator().get_allocator(), ssbo->allocation);
        device_.get_allocator().destroy_buffer(ssbo);
    }

    vkDestroyDescriptorSetLayout(device_.get_device(), descriptor_set_layout, nullptr);
}

void UIPass::update(const float& delta_time, const uint16_t& current_frame)
{
    // UIElement dot = { // black dot, 5px size
    //     .fill = {0.0f, 0.0f, 0.0f, 1.0f},
    //     .stroke = {0.1f, 0.1f, 0.1f, 1.0f},
    //     .position = { swap_chain_extent.width / 2, swap_chain_extent.height / 2 },
    //     .radius = 5.0f,
    //     .stroke_width = 1.0f,
    // };

    // const uint32_t samples = 100;
    // const float resolution = 0.1;
    // const auto pi2 = 2 * 3.1415;
    // std::vector<UIElement> elements;
    // elements.reserve(samples);

    // const uint32_t scale_factor_x = 50;
    // const uint32_t scale_factor_y = 10;

    // for (uint32_t s = 0; s < samples; ++s)
    // {
    //     auto temp_shape = dot;
    //     auto step = s / resolution;
    //     temp_shape.position.x += pi2 * step * scale_factor_x;
    //     temp_shape.position.y += std::sin(temp_shape.position.x) * scale_factor_y;
    //     elements.push_back(temp_shape);
    // }

    std::vector<UIElement> elements = {
        { // Giant blue circle
            .fill = {0.0f, 0.0f, 1.0f, 1.0f},
            .stroke = {0.0f, 1.0f, 0.0f, 1.0f},
            .position = {400, 400},
            .radius = 100.0f,
            .stroke_width = 10.0f,
        },
        { // Little red circle
            .fill = {1.0f, 0.0f, 0.0f, 1.0f},
            .stroke = {0.0f, 1.0f, 0.0f, 1.0f},
            .position = {100, 100},
            .radius = 50.0f,
            .stroke_width = 5.0f,
        },
        { // Medium ?? circle (black outline/slightly transparent)
            .fill = {0.0f, 1.0f, 0.0f, 1.0f},
            .stroke = {0.0f, 0.0f, 0.0f, 1.0f},
            .position = {300, 100},
            .radius = 75.0f,
            .stroke_width = 2.0f,
        }
    };

    // Logic here to make rounded boxes

    ui_push_constant_ = {
        .screen_size = { screen_size.width, screen_size.height },
        .num_shapes = static_cast<uint32_t>(elements.size()),
        .delta_time = delta_time,
    };

    std::memcpy(ui_elements_mapped_[current_frame].data(), elements.data(), sizeof(UIElement) * elements.size());
}

void UIPass::record(CommandRecorder& recorder, const uint16_t& current_frame)
{
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline.pipeline_layout,
        0,
        1,
        &descriptor_sets[current_frame],
        0,
        nullptr
    );
    
    vkCmdPushConstants(
        command_buffer,
        pipeline.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(ui_push_constant_),
        &ui_push_constant_
    );

    vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

}