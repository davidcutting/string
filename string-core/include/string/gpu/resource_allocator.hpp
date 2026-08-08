#pragma once

#include <functional>
#include <unordered_map>
#include <vector>

#include <string/gpu/resource.hpp>
#include <utility>

#include <volk.h>
#include <vk_mem_alloc.h>

namespace string::gpu
{

struct ResourceAllocatorCreateInfo
{
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
};

class resource_allocator
{
    VmaAllocator allocator_;
    VkPhysicalDevice physical_device_;
    VkDevice device_;
    id_registry registry_{ 1 };   // id 0 is reserved as the "no resource" sentinel — see id_registry

    std::unordered_map<resource_id, allocated_buffer> buffers_;
    std::unordered_map<resource_id, allocated_image> images_;

public:
    explicit resource_allocator(const ResourceAllocatorCreateInfo& info);
    ~resource_allocator();

    auto create_resource(const buffer_info& info) -> resource_id;
    auto create_resource(const image_info& info) -> resource_id;
    auto create_staging(VkDeviceSize size) -> resource_id;
    void destroy_resource(resource_id id);

    // A SUB-RESOURCE view of an existing image, registered as a resource in its own right (brief 20).
    // It owns a VkImageView over the given mip/layer range and shares the source's VkImage and
    // allocation — so `descriptor_table::bind(view_id, STORAGE_IMAGE)` works through the unmodified
    // five-function API, and a HiZ pyramid's per-mip writes or an IBL cubemap's per-face writes need
    // no synthetic ids. destroy_resource() on a view destroys only the view.
    struct view_range
    {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        std::uint32_t base_mip = 0;
        std::uint32_t mip_count = 1;
        std::uint32_t base_layer = 0;
        std::uint32_t layer_count = 1;
        // 2D_ARRAY rather than the source's default view type, which is what a compute shader needs
        // to storage-write individual cube faces.
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    };
    auto create_view(resource_id source, const view_range& range) -> resource_id;

    auto get_buffer(resource_id id) const -> const allocated_buffer&;
    auto get_image(resource_id id) const -> const allocated_image&;

    // Re-point an existing image at a different sampler configuration. The one caller that needs it
    // is texture streaming: `min_lod` tracks how much of a partially-resident mip chain has actually
    // been uploaded, so it changes over the image's life while everything else about the image stays
    // put. `descriptor_table::bind(id, TEXTURE)` afterwards is what publishes the change to the
    // bindless set — bind() reads the sampler straight off the image, so those two calls together are
    // the whole residency-rebinding verb.
    //
    // Safe to call with frames in flight: samplers are CACHED and shared (see sampler_for), never
    // destroyed until the allocator is, so the outgoing sampler stays valid for any command buffer
    // still holding a descriptor set that references it.
    void set_sampler(resource_id id, const sampler_info& info);

    void copy_data_to_buffer(const void* data, resource_id resource) const;

private:
    // Samplers are immutable configuration, so they are shared rather than owned per image: every
    // distinct sampler_info maps to exactly one VkSampler for the allocator's lifetime. That is what
    // makes set_sampler safe mid-flight, and it removes the per-image sampler lifetime that the
    // sub-resource-view sharing rule had to keep stepping around.
    std::vector<std::pair<sampler_info, VkSampler>> samplers_;
    auto sampler_for(const sampler_info& info) -> VkSampler;

    void create_image_sampler(allocated_image& allocated_image, const sampler_info& info);
    void create_image_view(allocated_image& allocated_image, VkImageAspectFlags aspect_flags);
};

struct deletion_queue
{
    std::deque<std::function<void()>> deletors;

    void push_function(std::function<void()>&& function)
    {
        deletors.push_back(std::move(function));
    }

    void flush()
    {
        // reverse iterate the deletion queue to execute all the functions
        for (auto it = deletors.rbegin(); it != deletors.rend(); it++)
        {
            (*it)(); // call functors
        }

        deletors.clear();
    }
};

} // namespace string::gpu