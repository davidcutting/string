#pragma once

#include <cstdint>
#include <stack>

#define VK_NO_PROTOTYPES
#include <vk_mem_alloc.h>
#include <volk.h>

namespace string::gpu
{

enum class stream_status : std::uint8_t
{
    WANTED,
    STREAMING,
    CACHED,
    UNWANTED
};

enum class resource_priority : std::uint8_t
{
    LAZY,
    IMMEDIATE
};

using resource_id = std::uint64_t;

// The acquired swapchain image's id. The one sentinel that survives: its backing genuinely is not
// known until the frame acquires it, which is what `persistent_image_info::swapchain` declares.
// (COLOR_TARGET / DEPTH_TARGET are gone — passes name the app's declared handles now, so there is
// nothing left for a renderer-owned sentinel to stand in for.)
constexpr resource_id SWAPCHAIN_TARGET = ~resource_id{0};

struct image_view;

// The LOGICAL resource vocabulary — the only resource types pass code ever names (brief 20/16).
// These ARE the reference doc's TaskImage/TaskBuffer under our names; there is no additional
// task-resource layer. The stored value indexes the owning `frame_graph`'s resource table, never a
// resource_id and never a VkImage — do not construct these by hand.
//
//   image / buffer     logical handle, declared to and resolved by the graph
//        |  frame_graph maps logical -> physical (per frame slot, per backing)
//   resource_id        physical slot handle (the allocator is kind-agnostic)
//        |  allocator maps slot -> concrete
//   allocated_image / allocated_buffer
//
// The clean names sit on the LOGICAL layer deliberately: with full virtualization the author only
// ever touches that layer, and `allocated_*` already claims the concrete-backing qualifier.
struct image
{
    static constexpr std::uint32_t invalid = ~std::uint32_t{ 0 };
    std::uint32_t index = invalid;
    bool valid() const { return index != invalid; }
    bool operator==(const image& o) const { return index == o.index; }

    // Sub-resource slices. A slice is a first-class thing the graph tracks state for and the
    // allocator can back with its own view + descriptor slot — this is what the HiZ pyramid's
    // per-mip writes and the IBL cubemap's per-face/per-mip writes declare, instead of the
    // synthetic-id storage-view hack they used to need.
    image_view mip(std::uint32_t level) const;
    image_view mips(std::uint32_t base, std::uint32_t count) const;
    image_view layer(std::uint32_t index) const;
    image_view whole() const;
};

struct buffer
{
    static constexpr std::uint32_t invalid = ~std::uint32_t{ 0 };
    std::uint32_t index = invalid;
    bool valid() const { return index != invalid; }
    bool operator==(const buffer& o) const { return index == o.index; }
};

// A slice of an `image` — the reference doc's TaskImageView. A whole-image view (the default) and a
// single-mip view are the same type, so declarations read uniformly and the graph tracks state per
// slice without a second vocabulary.
struct image_view
{
    static constexpr std::uint32_t all = ~std::uint32_t{ 0 };
    image img{};
    std::uint32_t base_mip = 0;
    std::uint32_t mip_count = all;
    std::uint32_t base_layer = 0;
    std::uint32_t layer_count = all;

    bool valid() const { return img.valid(); }
    bool whole_image() const { return base_mip == 0 && mip_count == all && base_layer == 0 && layer_count == all; }
    bool operator==(const image_view& o) const
    {
        return img == o.img && base_mip == o.base_mip && mip_count == o.mip_count
            && base_layer == o.base_layer && layer_count == o.layer_count;
    }
};

inline image_view image::mip(std::uint32_t level) const { return { *this, level, 1, 0, image_view::all }; }
inline image_view image::mips(std::uint32_t base, std::uint32_t count) const { return { *this, base, count, 0, image_view::all }; }
inline image_view image::layer(std::uint32_t idx) const { return { *this, 0, image_view::all, idx, 1 }; }
inline image_view image::whole() const { return { *this }; }

// How an image's sampler is configured. Brief 20: the allocator creates a sampler for EVERY image
// (it always did), but hardcoded exactly one configuration — LINEAR/REPEAT/aniso — so any pass
// wanting NEAREST or CLAMP_TO_EDGE had to create its own VkSampler and then force it into the
// descriptor set through update_texture(). Making the configuration a property of the image is what
// lets bind() be sufficient on its own again, and puts the sampler's lifetime with the image that
// owns it. Defaults reproduce the previous hardcoded configuration exactly.
struct sampler_info
{
    VkFilter mag_filter = VK_FILTER_LINEAR;
    VkFilter min_filter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    bool anisotropy = true;
    // Depth-compare sampling (shadow PCF): when set, the sampler is created with compareEnable and
    // this op, so a shader can use a SamplerComparisonState against a depth image.
    // These two defaults are inert (compare_op is ignored while compare_enable is false; border_color
    // only applies under CLAMP_TO_BORDER) but are set to the exact values the allocator previously
    // hardcoded, so the VkSamplerCreateInfo is literally identical for every existing caller.
    VkCompareOp compare_op = VK_COMPARE_OP_ALWAYS;
    bool compare_enable = false;
    VkBorderColor border_color = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    // Coarsest mip the sampler is allowed to reach — i.e. the finest level whose memory somebody has
    // actually written. It exists for PARTIALLY RESIDENT images: a streamed texture is created at its
    // full mip count but only its coarse tail is uploaded, and sampling a level that was never
    // uploaded reads whatever the allocation happened to contain (zeros on this driver: black albedo,
    // zero roughness — a sky mirror where a brick wall should be). The streamer raises detail by
    // LOWERING this as finer levels land, through resource_allocator::set_sampler.
    float min_lod = 0.0f;

    bool operator==(const sampler_info&) const = default;
};

struct image_info
{
    VkExtent3D extent;
    VkFormat format;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect_flags;
    VmaMemoryUsage memory_usage;
    VmaAllocationCreateFlags allocation_flags;
    // Sampler configuration for this image (brief 20). Defaulted to the configuration the allocator
    // previously hardcoded, so every existing call site is unchanged.
    sampler_info sampler{};
    // Number of mip levels; >1 means the image is created with a full/partial mip chain (the
    // uploader generates the smaller levels by blitting). Requires TRANSFER_SRC usage too.
    uint32_t mip_levels = 1;
    // MSAA sample count. >1 for multisampled render targets (which are resolved to a 1-sample
    // image for sampling); such images can't have a mip chain and aren't sampled directly.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    // Cube map (brief 07: the runtime sky IBL environment). Creates the image with 6 array
    // layers + CUBE_COMPATIBLE and a CUBE default view, so it can be sampled as a SamplerCube
    // through the bindless table. Per-mip / per-face storage views are declared as `image_view`
    // slices and backed by the graph, which is what retired the synthetic-id storage-view hack.
    bool cube = false;
};

struct allocated_image
{
    resource_id id;
    VkImage image;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
    VkExtent3D extent;
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;   // 6 for cube maps (default view is then CUBE)
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;   // >1 for multisampled render targets
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
    // A sub-resource VIEW of another image (resource_allocator::create_view): owns `view` only and
    // borrows `image`/`allocation` from its source, so destroying it must not touch either.
    bool is_view = false;
};

struct buffer_info
{
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    VmaMemoryUsage memory_usage;
    VmaAllocationCreateFlags allocation_flags;
};

struct allocated_buffer
{
    resource_id id;
    VkBuffer buffer;
    VkDeviceSize size;
    VmaAllocation allocation;
    VmaAllocationInfo allocation_info;
    // GPU virtual address for shader access, non-zero only when the buffer was created with
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT. Lets shaders read the buffer by pointer
    // (buffer_reference) instead of a bound descriptor — the basis for GPU-driven draws.
    VkDeviceAddress device_address = 0;
};

class id_registry
{
    resource_id capacity_ = 0;
    std::stack<resource_id> free_list_;
public:
    // `first` reserves the ids below it as sentinels. The resource allocator passes 1, because
    // resource_id 0 is this codebase's universal "no resource" value: members are declared
    // `resource_id foo_ = 0;` and guarded with `if (id != 0)`. Handing 0 out as a REAL id meant a
    // pass whose optional resource was never created still destroyed id 0 in its dtor — i.e. someone
    // else's buffer. Descriptor slots keep the default 0: there, slot 0 is a genuine slot.
    explicit id_registry(resource_id first = 0) : capacity_(first) {}

    auto get_id() -> resource_id
    {
        resource_id new_id;
        if (free_list_.empty())
        {
            new_id = capacity_;
            capacity_++;
            return new_id;
        }
        new_id = free_list_.top();
        free_list_.pop();
        return new_id;
    }
    void release_id(resource_id id)
    {
        free_list_.push(id);
    }
};

}