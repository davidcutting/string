#include <gtest/gtest.h>

#include <string/vulkan/resource_usage.hpp>

using namespace String;

// The model is constexpr, so mistakes in the Access -> (mask, layout) mapping surface at
// compile time as well as under the runtime checks below.
static_assert(is_write(Access::ColorWrite));
static_assert(is_write(Access::DepthWrite));
static_assert(is_write(Access::StorageWrite));
static_assert(is_write(Access::TransferWrite));
static_assert(!is_write(Access::SampledRead));
static_assert(!is_write(Access::DepthRead));
static_assert(!is_write(Access::StorageRead));
static_assert(!is_write(Access::VertexRead));
static_assert(!is_write(Access::IndexRead));
static_assert(!is_write(Access::TransferRead));

static_assert(access_scope(Access::ColorWrite).layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
static_assert(access_scope(Access::SampledRead).layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
static_assert(access_scope(Access::StorageRead).layout == VK_IMAGE_LAYOUT_UNDEFINED);

TEST(ResourceUsage, WriteClassification)
{
    EXPECT_TRUE(is_write(Access::ColorWrite));
    EXPECT_TRUE(is_write(Access::DepthWrite));
    EXPECT_FALSE(is_write(Access::SampledRead));
    EXPECT_FALSE(is_write(Access::DepthRead));
    EXPECT_FALSE(is_write(Access::VertexRead));
}

TEST(ResourceUsage, AccessScopeMapping)
{
    EXPECT_EQ(access_scope(Access::ColorWrite).access, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(access_scope(Access::ColorWrite).layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    EXPECT_EQ(access_scope(Access::SampledRead).access, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    EXPECT_EQ(access_scope(Access::SampledRead).layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Buffer usages carry no image layout.
    EXPECT_EQ(access_scope(Access::StorageRead).layout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(access_scope(Access::VertexRead).access, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    EXPECT_EQ(access_scope(Access::IndexRead).access, VK_ACCESS_2_INDEX_READ_BIT);
}
