#include <gtest/gtest.h>

#include <cstring>

#include <string/debug_draw.hpp>
#include <string/core/png_writer.hpp>

namespace
{
using namespace string;

TEST(DebugDraw, LineAccumulatesTwoVertices)
{
    debug::context().clear();
    debug::line({ 0, 0, 0 }, { 1, 0, 0 }, { 255, 0, 0, 255 });
    EXPECT_EQ(debug::context().depth_vertices().size(), 2u);
    EXPECT_EQ(debug::context().overlay_vertices().size(), 0u);
    // rgba packing: r|g<<8|b<<16|a<<24
    EXPECT_EQ(debug::context().depth_vertices()[0].rgba, 0xFF0000FFu);
}

TEST(DebugDraw, OverlayRoutesSeparately)
{
    debug::context().clear();
    debug::line_overlay({ 0, 0, 0 }, { 0, 1, 0 });
    EXPECT_EQ(debug::context().depth_vertices().size(), 0u);
    EXPECT_EQ(debug::context().overlay_vertices().size(), 2u);
}

TEST(DebugDraw, AabbIsTwelveEdges)
{
    debug::context().clear();
    debug::aabb({ -1, -1, -1 }, { 1, 1, 1 });
    EXPECT_EQ(debug::context().depth_vertices().size(), 24u);  // 12 edges * 2 verts
}

TEST(DebugDraw, ClearResets)
{
    debug::line({ 0, 0, 0 }, { 1, 1, 1 });
    debug::text3d({ 0, 0, 0 }, "hi");
    debug::context().clear();
    EXPECT_EQ(debug::context().depth_vertices().size(), 0u);
    EXPECT_EQ(debug::context().labels().size(), 0u);
}

TEST(PngWriter, EmitsValidSignatureAndChunks)
{
    const uint8_t pixels[4 * 3] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 };  // 2x2 RGB
    const std::vector<uint8_t> png = core::png::encode(pixels, 2, 2, 3);
    ASSERT_GE(png.size(), 8u);
    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    for (int i = 0; i < 8; ++i) EXPECT_EQ(png[i], sig[i]);
    // IHDR chunk tag immediately after the length (bytes 12..15).
    EXPECT_EQ(png[12], 'I'); EXPECT_EQ(png[13], 'H');
    EXPECT_EQ(png[14], 'D'); EXPECT_EQ(png[15], 'R');
    // Ends with IEND.
    const char iend[4] = { 'I', 'E', 'N', 'D' };
    bool found = false;
    for (std::size_t i = 0; i + 4 <= png.size(); ++i)
        if (std::memcmp(png.data() + i, iend, 4) == 0) { found = true; break; }
    EXPECT_TRUE(found);
}

}  // namespace
