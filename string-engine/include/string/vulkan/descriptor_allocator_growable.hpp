#pragma once

#include <cstdint>
#include <vector>
#include <deque>

#include <volk.h>

namespace String
{

class DescriptorLayoutBuilder
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;

public:
    auto add_binding(uint32_t binding, VkDescriptorType type) -> DescriptorLayoutBuilder&;
    auto clear() -> DescriptorLayoutBuilder&;
    auto build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0) -> VkDescriptorSetLayout;
};

struct DescriptorWriter
{
    std::deque<VkDescriptorImageInfo> image_infos;
    std::deque<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkWriteDescriptorSet> writes;

    void write_image(int binding, VkImageView image, VkSampler sampler, VkImageLayout layout, VkDescriptorType type);
    void write_buffer(int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type);
    void clear();
    void update_set(VkDevice device, VkDescriptorSet set);
};

struct PoolSizeRatio
{
    VkDescriptorType type;
    float ratio;
};

class DescriptorAllocatorGrowable
{
    VkDevice device_;
    std::vector<PoolSizeRatio> ratios;
	std::vector<VkDescriptorPool> full_pools;
	std::vector<VkDescriptorPool> ready_pools;
	uint32_t sets_per_pool;

public:
    DescriptorAllocatorGrowable() = default;

    void init(const VkDevice& device, uint32_t initial_sets, const std::vector<PoolSizeRatio>& pool_ratios);

	void clear_pools();
	void destroy_pools();
    auto allocate(VkDescriptorSetLayout layout, void* pNext = nullptr) -> VkDescriptorSet;

private:
    auto get_pool() -> VkDescriptorPool;
    auto create_pool(uint32_t set_count, const std::vector<PoolSizeRatio>& pool_ratios) -> VkDescriptorPool;
};

}