#include <gtest/gtest.h>

#include <string/core/layout.hpp>

using namespace string;
using namespace string::literals;

namespace
{
constexpr element box(sizing s, id identifier = {})
{
    element e{};
    e.sizing = s;
    e.id = identifier;
    return e;
}
}  // namespace

// --- basic flow --------------------------------------------------------------------------

TEST(Layout, FixedRow)
{
    layout_builder b;
    b.begin(format{ .gap = 5, .direction = direction::HORIZONTAL })
         .add_element(box(size_fixed(10, 10)))
         .add_element(box(size_fixed(20, 10)))
     .end();

    const auto n = b.nodes();
    ASSERT_EQ(n.size(), 3u);
    EXPECT_EQ(n[0].box.dimension.width, 35);   // 10 + 20 + gap 5
    EXPECT_EQ(n[0].box.dimension.height, 10);
    EXPECT_EQ(n[1].box.x, 0);
    EXPECT_EQ(n[2].box.x, 15);
}

TEST(Layout, FixedColumn)
{
    layout_builder b;
    b.begin(format{ .gap = 4, .direction = direction::VERTICAL })
         .add_element(box(size_fixed(10, 10)))
         .add_element(box(size_fixed(10, 20)))
     .end();

    const auto n = b.nodes();
    EXPECT_EQ(n[0].box.dimension.height, 34);   // 10 + 20 + gap 4
    EXPECT_EQ(n[1].box.y, 0);
    EXPECT_EQ(n[2].box.y, 14);
}

TEST(Layout, PaddingOffsetsChildren)
{
    layout_builder b;
    b.begin(box(size_fixed(100, 50)),
            format{ .padding = { .left = 10, .top = 5 }, .direction = direction::HORIZONTAL })
         .add_element(box(size_fixed(10, 10)))
     .end();

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.x, 10);
    EXPECT_EQ(n[1].box.y, 5);
}

TEST(Layout, Nested)
{
    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL })
         .add_element(box(size_fixed(10, 10)))
         .begin(format{ .gap = 5, .direction = direction::HORIZONTAL })
             .add_element(box(size_fixed(10, 10)))
             .add_element(box(size_fixed(20, 10)))
         .end()
     .end();

    const auto n = b.nodes();
    ASSERT_EQ(n.size(), 5u);
    EXPECT_EQ(n[0].box.dimension.width, 35);    // widest row
    EXPECT_EQ(n[0].box.dimension.height, 20);   // stacked
    EXPECT_EQ(n[2].box.y, 10);                   // inner row below first child
    EXPECT_EQ(n[4].box.x, 15);                   // second item of inner row
    EXPECT_EQ(n[4].box.y, 10);
}

// --- flex: grow / shrink -----------------------------------------------------------------

TEST(Layout, GrowSplitsEqually)
{
    layout_builder b;
    b.begin(box(size_fixed(100, 10)), format{ .direction = direction::HORIZONTAL })
         .add_element(box({ grow(), fixed(10) }))
         .add_element(box({ grow(), fixed(10) }))
     .end();

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 50);
    EXPECT_EQ(n[2].box.dimension.width, 50);
    EXPECT_EQ(n[2].box.x, 50);
}

TEST(Layout, GrowRespectsMax)
{
    layout_builder b;
    b.begin(box(size_fixed(100, 10)), format{ .direction = direction::HORIZONTAL })
         .add_element(box({ grow(0, 20), fixed(10) }))
         .add_element(box({ grow(), fixed(10) }))
     .end();

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 20);   // capped
    EXPECT_EQ(n[2].box.dimension.width, 80);   // gets the rest
}

TEST(Layout, GrowStretchesCrossAxis)
{
    layout_builder b;
    b.begin(box(size_fixed(50, 40)), format{ .direction = direction::HORIZONTAL })
         .add_element(box({ fixed(10), grow() }))
     .end();

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 10);
    EXPECT_EQ(n[1].box.dimension.height, 40);   // stretched to container
}

TEST(Layout, ShrinkFlexibleChildren)
{
    element a = box(size_fixed(15, 10));
    element flexible = box({ fit(10), fixed(10) });
    flexible.sizing.width.min = 5;

    layout_builder b;
    b.begin(box(size_fixed(20, 10)), format{ .direction = direction::HORIZONTAL })
         .add_element(a)
         .add_element(flexible)
     .end();

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 15);   // FIXED: untouched
    EXPECT_EQ(n[2].box.dimension.width, 5);    // FIT: shrunk 10 -> 5 to reclaim the overflow
}

TEST(Layout, FixedNeverShrinks)
{
    layout_builder b;
    b.begin(box(size_fixed(20, 10)), format{ .direction = direction::HORIZONTAL })
         .add_element(box(size_fixed(20, 10)))
         .add_element(box(size_fixed(20, 10)))
     .end();

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 20);   // overflows rather than shrink
    EXPECT_EQ(n[2].box.dimension.width, 20);
}

// --- justification (main axis) -----------------------------------------------------------

namespace
{
// Returns {child0.x, child1.x} for a 100-wide row of two 10-wide boxes under `j`.
std::pair<int, int> justify_row(justification j)
{
    layout_builder b;
    b.begin(box(size_fixed(100, 10)), format{ .direction = direction::HORIZONTAL, .justify = j })
         .add_element(box(size_fixed(10, 10)))
         .add_element(box(size_fixed(10, 10)))
     .end();
    const auto n = b.nodes();
    return { n[1].box.x, n[2].box.x };
}
}  // namespace

TEST(Layout, JustifyStart)       { EXPECT_EQ(justify_row(justification::START),         (std::pair{ 0, 10 })); }
TEST(Layout, JustifyCenter)      { EXPECT_EQ(justify_row(justification::CENTER),        (std::pair{ 40, 50 })); }
TEST(Layout, JustifyEnd)         { EXPECT_EQ(justify_row(justification::END),           (std::pair{ 80, 90 })); }
TEST(Layout, JustifySpaceBetween){ EXPECT_EQ(justify_row(justification::SPACE_BETWEEN), (std::pair{ 0, 90 })); }
TEST(Layout, JustifySpaceAround) { EXPECT_EQ(justify_row(justification::SPACE_AROUND),  (std::pair{ 20, 70 })); }
TEST(Layout, JustifySpaceEvenly) { EXPECT_EQ(justify_row(justification::SPACE_EVENLY),  (std::pair{ 26, 62 })); }

// --- alignment (cross axis) --------------------------------------------------------------

namespace
{
int align_child_y(alignment a)
{
    layout_builder b;
    b.begin(box(size_fixed(100, 40)), format{ .alignment = a, .direction = direction::HORIZONTAL })
         .add_element(box(size_fixed(10, 10)))
     .end();
    return b.nodes()[1].box.y;
}
}  // namespace

TEST(Layout, AlignTop)    { EXPECT_EQ(align_child_y(alignment::TOP), 0); }
TEST(Layout, AlignCenter) { EXPECT_EQ(align_child_y(alignment::CENTER), 15); }
TEST(Layout, AlignBottom) { EXPECT_EQ(align_child_y(alignment::BOTTOM), 30); }

// --- viewport / measurement --------------------------------------------------------------

TEST(Layout, ViewportFillsRoot)
{
    layout_builder b;
    b.begin(box(size_grow()), format{ .direction = direction::HORIZONTAL })
         .add_element(box(size_grow()))
     .end(dimension{ 100, 20 });

    const auto n = b.nodes();
    EXPECT_EQ(n[0].box.dimension.width, 100);
    EXPECT_EQ(n[1].box.dimension.width, 100);
}

TEST(Layout, MeasurementHookSizesLeaves)
{
    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL })
         .add_element(box(size_fit()))
     .end([](const element&) -> dimension { return { 30, 12 }; });

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 30);
    EXPECT_EQ(n[1].box.dimension.height, 12);
}

// --- id lookup / hit-testing -------------------------------------------------------------

TEST(Layout, FindById)
{
    layout_builder b;
    b.begin(format{ .gap = 5, .direction = direction::HORIZONTAL })
         .add_element(box(size_fixed(10, 10), "a"_id))
         .add_element(box(size_fixed(20, 10), "b"_id))
     .end();

    const layout_node* found = b.find("b"_id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->box.x, 15);
    EXPECT_EQ(b.find("missing"_id), nullptr);
}

TEST(Layout, HitTestReturnsTopMost)
{
    layout_builder b;
    b.begin(format{ .gap = 5, .direction = direction::HORIZONTAL })
         .add_element(box(size_fixed(10, 10), "a"_id))
         .add_element(box(size_fixed(20, 10), "b"_id))
     .end();

    EXPECT_EQ(b.hit_test(5, 5), b.find("a"_id));
    EXPECT_EQ(b.hit_test(20, 5), b.find("b"_id));
    EXPECT_EQ(b.hit_test(200, 200), nullptr);
}

// --- output paths / lifecycle ------------------------------------------------------------

TEST(Layout, EvaluateLayoutAtRuntime)
{
    const auto ui = evaluate_layout<[](layout_builder& b) {
        b.begin(format{ .gap = 5, .direction = direction::HORIZONTAL })
             .add_element(box(size_fixed(10, 10)))
             .add_element(box(size_fixed(20, 10)))
         .end();
    }>();

    ASSERT_EQ(ui.size(), 3u);
    EXPECT_EQ(ui[2].bounding_box.x, 15);
}

TEST(Layout, ScopeGuardCloses)
{
    layout_builder b;
    {
        auto row = b.container(format{ .gap = 5, .direction = direction::HORIZONTAL });
        b.add_element(box(size_fixed(10, 10)));
        b.add_element(box(size_fixed(20, 10)));
    }

    const auto n = b.nodes();
    ASSERT_EQ(n.size(), 3u);
    EXPECT_EQ(n[2].box.x, 15);
}

TEST(Layout, ClearReuses)
{
    layout_builder b;
    b.begin(format{ .direction = direction::HORIZONTAL }).add_element(box(size_fixed(10, 10))).end();
    ASSERT_EQ(b.nodes().size(), 2u);

    b.clear();
    EXPECT_EQ(b.nodes().size(), 0u);

    b.begin(format{ .direction = direction::HORIZONTAL })
         .add_element(box(size_fixed(10, 10)))
         .add_element(box(size_fixed(10, 10)))
     .end();
    EXPECT_EQ(b.nodes().size(), 3u);
}

TEST(Layout, IdHashIsFnv1a)
{
    EXPECT_EQ("panel"_id.hash, fnv1a("panel"));
    EXPECT_EQ(make_id("panel").hash, fnv1a("panel"));
}
