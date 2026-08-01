#include <gtest/gtest.h>

#include <string/core/layout.hpp>

#include <utility>

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

// --- Weighted GROW (brief 12 M2a) ---------------------------------------------------------------
// Split ratios need PROPORTIONAL sizing: no combination of min/max can express "70/30" without
// already knowing the container's resolved size, which an author does not have.

namespace
{
// Two weighted growers in a fixed-width row; returns their resolved widths.
std::pair<int, int> split_widths(std::uint16_t wa, std::uint16_t wb, std::uint16_t total)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(total, 40);
    b.begin(root, string::format{ .direction = string::direction::HORIZONTAL });
    {
        string::element a{};
        a.id = string::make_id("a");
        a.sizing = { string::grow_weighted(wa), string::grow() };
        b.add_element(a);
        string::element c{};
        c.id = string::make_id("b");
        c.sizing = { string::grow_weighted(wb), string::grow() };
        b.add_element(c);
    }
    b.end(string::dimension{ total, 40 });
    return { b.find(string::make_id("a").hash)->box.dimension.width,
             b.find(string::make_id("b").hash)->box.dimension.width };
}
}  // namespace

TEST(LayoutTest, EqualWeightsReproduceTheEvenSplit)
{
    const auto [a, c] = split_widths(1, 1, 400);
    EXPECT_EQ(a, 200);
    EXPECT_EQ(c, 200);
}

TEST(LayoutTest, WeightsSplitSurplusInProportion)
{
    const auto [a, c] = split_widths(7, 3, 1000);
    EXPECT_NEAR(a, 700, 2);
    EXPECT_NEAR(c, 300, 2);
    EXPECT_EQ(a + c, 1000);   // no surplus stranded
}

TEST(LayoutTest, WeightOrderDoesNotBiasTheSplit)
{
    // The mirrored weights must mirror the result. This is what fails if shares are computed
    // against the RUNNING leftover instead of a per-pass snapshot: the first child takes too much
    // either way, so 7/3 and 3/7 would not be symmetric.
    const auto [a1, b1] = split_widths(7, 3, 1000);
    const auto [a2, b2] = split_widths(3, 7, 1000);
    EXPECT_NEAR(a1, b2, 2);
    EXPECT_NEAR(b1, a2, 2);
}

TEST(LayoutTest, ZeroWeightIsTreatedAsOne)
{
    // A grower that could never receive anything is a silent layout hole; FIXED/FIT already say
    // "do not grow".
    const auto [a, c] = split_widths(0, 1, 400);
    EXPECT_EQ(a, 200);
    EXPECT_EQ(c, 200);
}

TEST(LayoutTest, WeightedGrowStillRespectsMaxAndRedistributes)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(1000, 40);
    b.begin(root, string::format{ .direction = string::direction::HORIZONTAL });
    {
        string::element a{};
        a.id = string::make_id("capped");
        a.sizing = { string::grow_weighted(7, 0, 100), string::grow() };   // wants 700, capped at 100
        b.add_element(a);
        string::element c{};
        c.id = string::make_id("rest");
        c.sizing = { string::grow_weighted(3), string::grow() };
        b.add_element(c);
    }
    b.end(string::dimension{ 1000, 40 });

    // The capped child's unused share must flow to the other, not be stranded — which is why the
    // distribution is a loop rather than a single weighted division.
    EXPECT_EQ(b.find(string::make_id("capped").hash)->box.dimension.width, 100);
    EXPECT_EQ(b.find(string::make_id("rest").hash)->box.dimension.width, 900);
}

// --- PERCENT sizing (brief 12 M2c fix) -----------------------------------------------------------
// The primitive a resizable split needs. GROW cannot do this job: it starts at CONTENT size and only
// shares the SURPLUS, so pixels -> ratio -> pixels does not round-trip and a splitter jumps on click.

TEST(LayoutTest, PercentIgnoresContentSize)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(1000, 100);
    b.begin(root, string::format{ .direction = string::direction::HORIZONTAL });
    {
        // Deliberately different content sizes: percent must ignore them entirely.
        string::element a{};
        a.id = string::make_id("a");
        a.sizing = { string::percent(700), string::grow() };
        b.begin(a, string::format{});
        {
            string::element filler{};
            filler.sizing = string::size_fixed(400, 20);
            b.add_element(filler);
        }
        b.end();

        string::element c{};
        c.id = string::make_id("b");
        c.sizing = { string::percent(300), string::grow() };
        b.add_element(c);
    }
    b.end(string::dimension{ 1000, 100 });

    EXPECT_EQ(b.find(string::make_id("a").hash)->box.dimension.width, 700);
    EXPECT_EQ(b.find(string::make_id("b").hash)->box.dimension.width, 300);
}

// Percent shares what is left AFTER definite siblings — which is exactly the splitter case: two
// regions either side of a fixed-width grip.
TEST(LayoutTest, PercentSharesTheSpaceLeftByDefiniteSiblings)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(806, 100);
    b.begin(root, string::format{ .direction = string::direction::HORIZONTAL });
    {
        string::element a{};
        a.id = string::make_id("a");
        a.sizing = { string::percent(500), string::grow() };
        b.add_element(a);

        string::element grip{};
        grip.sizing = string::size_fixed(6, 100);   // the splitter
        b.add_element(grip);

        string::element c{};
        c.id = string::make_id("b");
        c.sizing = { string::percent(500), string::grow() };
        b.add_element(c);
    }
    b.end(string::dimension{ 806, 100 });

    const int wa = b.find(string::make_id("a").hash)->box.dimension.width;
    const int wb = b.find(string::make_id("b").hash)->box.dimension.width;
    EXPECT_EQ(wa, 400);
    EXPECT_EQ(wb, 400);
    EXPECT_EQ(wa + wb + 6, 806);   // tiles exactly, no stray pixel at the seam
}

// The rounding remainder goes to the last percent child, so an odd split still tiles exactly.
TEST(LayoutTest, PercentTilesExactlyOnOddSplits)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(999, 100);
    b.begin(root, string::format{ .direction = string::direction::HORIZONTAL });
    {
        string::element a{};
        a.id = string::make_id("a");
        a.sizing = { string::percent(565), string::grow() };
        b.add_element(a);
        string::element c{};
        c.id = string::make_id("b");
        c.sizing = { string::percent(435), string::grow() };
        b.add_element(c);
    }
    b.end(string::dimension{ 999, 100 });

    const int wa = b.find(string::make_id("a").hash)->box.dimension.width;
    const int wb = b.find(string::make_id("b").hash)->box.dimension.width;
    EXPECT_EQ(wa + wb, 999);
}
