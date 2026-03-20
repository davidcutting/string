#include <cstdint>
#include <filesystem>
#include <cstring>
#include <span>

#include <string/vulkan/pipelines/hello_slang_pipeline.hpp>
#include <string/vulkan/passes/hello_slang_pass.hpp>

namespace String
{

HelloSlang::HelloSlang(Device& device, const std::filesystem::path& resources_path, const uint16_t& frames_in_flight)
: device_(device)
{
    // clang-format off
    VkDescriptorSetLayoutBinding buffer0_layout_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr
    };

    VkDescriptorSetLayoutBinding buffer1_layout_binding = {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr
    };

    VkDescriptorSetLayoutBinding result_buffer_layout_binding = {
        .binding = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr
    };
    // clang-format on

    std::vector<VkDescriptorSetLayoutBinding> bindings = {buffer0_layout_binding, buffer1_layout_binding, result_buffer_layout_binding};

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

    std::vector<VkDescriptorSetLayout> hello_slang_layouts(frames_in_flight, descriptor_set_layout);
    VkDescriptorSetAllocateInfo hello_slang_descriptor_set_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = static_cast<uint32_t>(frames_in_flight),
        .pSetLayouts = hello_slang_layouts.data()
    };
    descriptor_sets.resize(frames_in_flight);

    if (vkAllocateDescriptorSets(device_.get_device(), &hello_slang_descriptor_set_alloc_info, descriptor_sets.data()) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate HelloSlang descriptor sets!");
    }

    for (size_t i = 0; i < frames_in_flight; i++)
    {
        VkDescriptorBufferInfo hello_slang_buffer0_info = {
            .buffer = hello_slang_buffer0_[i]->buffer,
            .offset = 0,
            .range = hello_slang_buffer0_[i]->buffer_size
        };

        VkDescriptorBufferInfo hello_slang_buffer1_info = {
            .buffer = hello_slang_buffer1_[i]->buffer,
            .offset = 0,
            .range = hello_slang_buffer1_[i]->buffer_size
        };

        VkDescriptorBufferInfo hello_slang_result_info = {
            .buffer = hello_slang_result_[i]->buffer,
            .offset = 0,
            .range = hello_slang_result_[i]->buffer_size
        };

        std::vector<VkWriteDescriptorSet> descriptor_writes = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = descriptor_sets[i],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &hello_slang_buffer0_info,
                .pTexelBufferView = nullptr,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = descriptor_sets[i],
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &hello_slang_buffer1_info,
                .pTexelBufferView = nullptr,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = descriptor_sets[i],
                .dstBinding = 2,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &hello_slang_result_info,
                .pTexelBufferView = nullptr,
            }
        };

        vkUpdateDescriptorSets(
            device_.get_device(),
            static_cast<uint32_t>(descriptor_writes.size()),
            descriptor_writes.data(), 0,
            nullptr);
    }

    hello_slang_pipeline_ = std::make_unique<HelloSlangPipeline>(
        resources_path,
        device,
        descriptor_set_layout
    );

    hello_slang_buffer0_.resize(frames_in_flight);
    hello_slang_buffer0_mapped_.resize(frames_in_flight);
    hello_slang_buffer1_.resize(frames_in_flight);
    hello_slang_buffer1_mapped_.resize(frames_in_flight);
    hello_slang_result_.resize(frames_in_flight);
    hello_slang_result_mapped_.resize(frames_in_flight);

    uint32_t num_numbers_lol = 100;
    VkDeviceSize buffer_size = sizeof(float) * num_numbers_lol;
    void* hello_slang_buffer_mapping;

    for (size_t i = 0; i < frames_in_flight; i++)
    {
        hello_slang_buffer0_[i] = device_.get_allocator().create_buffer(buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        vmaMapMemory(
            device_.get_allocator().get_allocator(),
            hello_slang_buffer0_[i]->allocation,
            &hello_slang_buffer_mapping
        );
        hello_slang_buffer0_mapped_[i] = std::span<float>(
            reinterpret_cast<float*>(hello_slang_buffer_mapping),
            num_numbers_lol
        );

        hello_slang_buffer1_[i] = device_.get_allocator().create_buffer(buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        vmaMapMemory(
            device_.get_allocator().get_allocator(),
            hello_slang_buffer1_[i]->allocation,
            &hello_slang_buffer_mapping
        );
        hello_slang_buffer1_mapped_[i] = std::span<float>(
            reinterpret_cast<float*>(hello_slang_buffer_mapping),
            num_numbers_lol
        );

        hello_slang_result_[i] = device_.get_allocator().create_buffer(buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        vmaMapMemory(
            device_.get_allocator().get_allocator(),
            hello_slang_result_[i]->allocation,
            &hello_slang_buffer_mapping
        );
        hello_slang_result_mapped_[i] = std::span<float>(
            reinterpret_cast<float*>(hello_slang_buffer_mapping),
            num_numbers_lol
        );
    }
}

HelloSlang::~HelloSlang()
{
    hello_slang_pipeline_.reset();

    for (auto& buff : hello_slang_buffer0_)
    {
        vmaUnmapMemory(device_.get_allocator().get_allocator(), buff->allocation);
        device_.get_allocator().destroy_buffer(buff);
    }
    for (auto& buff : hello_slang_buffer1_)
    {
        vmaUnmapMemory(device_.get_allocator().get_allocator(), buff->allocation);
        device_.get_allocator().destroy_buffer(buff);
    }
    for (auto& buff : hello_slang_result_)
    {
        vmaUnmapMemory(device_.get_allocator().get_allocator(), buff->allocation);
        device_.get_allocator().destroy_buffer(buff);
    }

    vkDestroyDescriptorSetLayout(device_.get_device(), descriptor_set_layout, nullptr);
}

void HelloSlang::update(const float& delta_time, const uint16_t& current_frame)
{
    static constexpr size_t hello_buffer_size = 1024;
    float buffer0[hello_buffer_size];
    float buffer1[hello_buffer_size];

    for (size_t i = 0; i < hello_buffer_size; ++i)
    {
        buffer0[i] = 1.0 * i;
        buffer1[i] = 1.0 * i;
    }

    std::memcpy(hello_slang_buffer0_mapped_[current_frame].data(), buffer0, sizeof(float) * hello_buffer_size);
    std::memcpy(hello_slang_buffer1_mapped_[current_frame].data(), buffer0, sizeof(float) * hello_buffer_size);
}

void HelloSlang::record(CommandRecorder& recorder, const uint16_t& current_frame)
{
    VkCommandBuffer& command_buffer = recorder.get_command_buffer();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, hello_slang_pipeline_->get_pipeline());
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        hello_slang_pipeline_->get_pipeline_layout(),
        0,
        1,
        &descriptor_sets[current_frame],
        0,
        nullptr
    );

    const uint32_t element_count = 1024;
    const uint32_t group_size = 64;
	vkCmdDispatch(command_buffer, (element_count + group_size - 1) / group_size, 1, 1);
}

}