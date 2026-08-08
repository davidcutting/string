#include <string/vulkan/resource_usage.hpp>

using namespace string;

// The access vocabulary's two invariants: which accesses are writes, and that every access maps to a
// layout consistent with whether it names a buffer or an image.
static_assert(is_write(access::color_write));
static_assert(is_write(access::depth_write));
static_assert(is_write(access::storage_write));
static_assert(is_write(access::transfer_write));
static_assert(is_write(access::storage_image_write));
static_assert(!is_write(access::sampled_read));
static_assert(!is_write(access::depth_read));
static_assert(!is_write(access::storage_read));
static_assert(!is_write(access::vertex_read));
static_assert(!is_write(access::index_read));
static_assert(!is_write(access::indirect_read));
static_assert(!is_write(access::transfer_read));
static_assert(!is_write(access::storage_image_read));

// A buffer access is exactly one whose layout does not apply.
static_assert(scope_of(access::storage_read).layout == VK_IMAGE_LAYOUT_UNDEFINED);
static_assert(scope_of(access::indirect_read).layout == VK_IMAGE_LAYOUT_UNDEFINED);
static_assert(scope_of(access::sampled_read).layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
static_assert(scope_of(access::storage_image_write).layout == VK_IMAGE_LAYOUT_GENERAL);

// Write scopes cover BOTH directions: attachment use also reads (LOAD_OP_LOAD, blending, resolve),
// and storage writes are commonly read-modify-write. A one-directional scope here is the ghosting
// bug class.
static_assert((scope_of(access::color_write).mask & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) != 0);
static_assert((scope_of(access::storage_write).mask & VK_ACCESS_2_SHADER_STORAGE_READ_BIT) != 0);
static_assert((scope_of(access::storage_image_write).mask & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT) != 0);
