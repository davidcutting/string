#include <stdexcept>
#include <string/vulkan/descriptor_allocator_growable.hpp>
#include <string/vulkan/device.hpp>
#include <vector>

namespace String
{

// Descriptor Layout Builder

auto DescriptorLayoutBuilder::add_binding(uint32_t binding, VkDescriptorType type) -> DescriptorLayoutBuilder&
{
    VkDescriptorSetLayoutBinding bind = {
        .binding = binding,
        .descriptorType = type,
        .descriptorCount = 1,
        .stageFlags = 0,
        .pImmutableSamplers = nullptr,
    };

    bindings.emplace_back(std::move(bind));
    return *this;
}

auto DescriptorLayoutBuilder::clear() -> DescriptorLayoutBuilder&
{
    bindings.clear();
    return *this;
}

auto DescriptorLayoutBuilder::build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext, VkDescriptorSetLayoutCreateFlags flags) -> VkDescriptorSetLayout
{
    for (auto& b : bindings) {
        b.stageFlags |= shaderStages;
    }

    VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.pNext = pNext;

    info.pBindings = bindings.data();
    info.bindingCount = (uint32_t)bindings.size();
    info.flags = flags;

    VkDescriptorSetLayout set;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &set) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create descriptor set layout.");
    }
    return set;
}

// Descriptor Writer

void DescriptorWriter::write_image(int binding,VkImageView image, VkSampler sampler,  VkImageLayout layout, VkDescriptorType type)
{
    VkDescriptorImageInfo& info = image_infos.emplace_back(VkDescriptorImageInfo{
		.sampler = sampler,
		.imageView = image,
		.imageLayout = layout
	});

	VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };

	write.dstBinding = binding;
	write.dstSet = VK_NULL_HANDLE; //left empty for now until we need to write it
	write.descriptorCount = 1;
	write.descriptorType = type;
	write.pImageInfo = &info;

	writes.push_back(write);
}

void DescriptorWriter::write_buffer(int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type)
{
	VkDescriptorBufferInfo& info = buffer_infos.emplace_back(VkDescriptorBufferInfo{
		.buffer = buffer,
		.offset = offset,
		.range = size
		});

	VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

	write.dstBinding = binding;
	write.dstSet = VK_NULL_HANDLE; //left empty for now until we need to write it
	write.descriptorCount = 1;
	write.descriptorType = type;
	write.pBufferInfo = &info;

	writes.push_back(write);
}

void DescriptorWriter::clear()
{
    image_infos.clear();
    writes.clear();
    buffer_infos.clear();
}

void DescriptorWriter::update_set(VkDevice device, VkDescriptorSet set)
{
    for (VkWriteDescriptorSet& write : writes) {
        write.dstSet = set;
    }

    vkUpdateDescriptorSets(device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

// Descriptor Allocator Growable

void DescriptorAllocatorGrowable::init(const VkDevice& device, uint32_t initial_sets, const std::vector<PoolSizeRatio>& pool_ratios)
{
    device_ = device;

    ratios.clear();
    
    for (auto r : pool_ratios) {
        ratios.push_back(r);
    }
	
    VkDescriptorPool new_pool = create_pool(initial_sets, pool_ratios);

    sets_per_pool = initial_sets * 1.5; //grow it next allocation

    ready_pools.push_back(new_pool);
}

void DescriptorAllocatorGrowable::clear_pools()
{ 
    for (auto p : ready_pools)
    {
        vkResetDescriptorPool(device_, p, 0);
    }

    for (auto p : full_pools)
    {
        vkResetDescriptorPool(device_, p, 0);
        ready_pools.push_back(p);
    }

    full_pools.clear();
}

void DescriptorAllocatorGrowable::destroy_pools()
{
	for (auto p : ready_pools)
    {
		vkDestroyDescriptorPool(device_, p, nullptr);
	}
    ready_pools.clear();

	for (auto p : full_pools)
    {
		vkDestroyDescriptorPool(device_, p, nullptr);
    }
    full_pools.clear();
}

VkDescriptorPool DescriptorAllocatorGrowable::get_pool()
{       
    VkDescriptorPool new_pool;
    if (ready_pools.size() != 0)
    {
        new_pool = ready_pools.back();
        ready_pools.pop_back();
    }
    else
    {
	    //need to create a new pool
	    new_pool = create_pool(sets_per_pool, ratios);
	    sets_per_pool = sets_per_pool * 1.5;

	    if (sets_per_pool > 4092)
        {
		    sets_per_pool = 4092;
	    }
    }   

    return new_pool;
}

VkDescriptorPool DescriptorAllocatorGrowable::create_pool(uint32_t set_count, const std::vector<PoolSizeRatio>& pool_ratios)
{
	std::vector<VkDescriptorPoolSize> pool_sizes;

	for (PoolSizeRatio ratio : pool_ratios)
    {
		pool_sizes.push_back({
			.type = ratio.type,
			.descriptorCount = static_cast<uint32_t>(ratio.ratio * set_count),
		});
	}

	VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = set_count,        
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };

	VkDescriptorPool new_pool;

	if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &new_pool) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create descriptor pool.");
    }

    return new_pool;
}

VkDescriptorSet DescriptorAllocatorGrowable::allocate(VkDescriptorSetLayout layout, void* pNext)
{
    //get or create a pool to allocate from
    VkDescriptorPool pool_to_use = get_pool();

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.pNext = pNext;
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = pool_to_use;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet descriptor_set;
	VkResult result = vkAllocateDescriptorSets(device_, &allocInfo, &descriptor_set);

    //allocation failed. Try again
    if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
    {
        full_pools.push_back(pool_to_use);
    
        pool_to_use = get_pool();
        allocInfo.descriptorPool = pool_to_use;

        if (vkAllocateDescriptorSets(device_, &allocInfo, &descriptor_set) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to allocate descriptor sets.");
        }
    }
  
    ready_pools.push_back(pool_to_use);
    return descriptor_set;
}

}