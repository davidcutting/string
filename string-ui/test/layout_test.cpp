#include <gtest/gtest.h>

#include <string/ui/layout.hpp>
#include <string/ui/interaction.hpp>

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

namespace
{
// A measurer is three operations now, not one callable — see the `measurer` concept: the unwrapped
// size, the shrink floor (widest word), and the height once wrapped to a given width. This stand-in
// says 30x12 of content made of 10-wide words, needing one 12px line per 30px of width.
struct stub_measure
{
    dimension operator()(const element&) const noexcept { return { 30, 12 }; }
    std::uint16_t min_width(const element&) const noexcept { return 10; }
    std::uint16_t height_at(const element&, std::uint16_t w) const noexcept
    {
        return static_cast<std::uint16_t>(12 * (w > 0 ? std::max(1, (30 + w - 1) / w) : 1));
    }
};
}  // namespace

TEST(Layout, MeasurementHookSizesLeaves)
{
    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL })
         .add_element(box(size_fit()))
     .end(stub_measure{});

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 30);
    EXPECT_EQ(n[1].box.dimension.height, 12);
}

// --- Text wrap ---------------------------------------------------------------------------------

// The seam between the two flex passes: width is final, so the height can be asked for at that
// width, and the answer propagates up to a FIT parent.
TEST(Layout, WrapTakesItsHeightFromTheResolvedWidth)
{
    element text = box(sizing{ grow(), fit() });
    text.wrap = true;

    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL })
         .add_element(text)
     .end(dimension{ 10, 200 }, stub_measure{});   // 10 wide -> three lines of 12

    const auto n = b.nodes();
    EXPECT_EQ(n[1].box.dimension.width, 10);
    EXPECT_EQ(n[1].box.dimension.height, 36);
}

TEST(Layout, WrapHeightReachesAFitAncestor)
{
    element text = box(sizing{ grow(), fit() });
    text.wrap = true;

    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL })
         .begin(box(sizing{ fixed(15), fit() }), format{ .direction = direction::VERTICAL })
             .add_element(text)
         .end()
     .end(dimension{ 300, 300 }, stub_measure{});

    const auto n = b.nodes();
    EXPECT_EQ(n[2].box.dimension.height, 24) << "two lines at width 15";
    EXPECT_EQ(n[1].box.dimension.height, 24) << "the FIT container must grow with its wrapped child";
}

// Shrink must not squeeze a wrapping element past its widest word — below that, wrapping cannot
// produce a line that fits and the renderer is forced to break a word in half.
TEST(Layout, WrapFloorsShrinkAtTheWidestWord)
{
    element text = box(sizing{ grow(), fit() });
    text.wrap = true;

    layout_builder b;
    b.begin(format{ .direction = direction::HORIZONTAL })
         .add_element(text)
         .add_element(box(sizing{ fixed(295), fit() }))   // hogs the row, forcing a shrink
     .end(dimension{ 300, 100 }, stub_measure{});

    EXPECT_GE(b.nodes()[1].box.dimension.width, 10) << "the widest word is the floor";
}

// The unwrapped path must be untouched: same tree without the flag keeps its one-line height.
TEST(Layout, WrapIsOptIn)
{
    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL })
         .add_element(box(sizing{ grow(), fit() }))
     .end(dimension{ 10, 200 }, stub_measure{});

    EXPECT_EQ(b.nodes()[1].box.dimension.height, 12);
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

    // Through the ONE hit test (ui::hit_test_layered). The layout builder deliberately has none of
    // its own — see layout.hpp.
    EXPECT_EQ(string::ui::hit_test_layered(b, 5, 5), b.find("a"_id));
    EXPECT_EQ(string::ui::hit_test_layered(b, 20, 5), b.find("b"_id));
    EXPECT_EQ(string::ui::hit_test_layered(b, 200, 200), nullptr);
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

// --- Parent-relative floating (brief 13 M4) -------------------------------------------------------
// Root-space floating suits world-anchored UI, but a container that computes its children's
// positions in its OWN coordinates (a graph canvas) would otherwise have to discover where it landed
// on screen — reading geometry back for something layout can just do.

TEST(LayoutTest, LocalFloatingIsRelativeToTheParentContentBox)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(400, 300);
    b.begin(root, string::format{ .direction = string::direction::VERTICAL });
    {
        string::element panel{};
        panel.id = string::make_id("panel");
        panel.sizing = string::size_fixed(200, 150);
        b.begin(panel, string::format{ .padding = { 10, 0, 20, 0 } });
        {
            string::element node{};
            node.id = string::make_id("node");
            node.sizing = string::size_fixed(20, 20);
            node.floating = true;
            node.float_local = true;
            node.float_x = 30;
            node.float_y = 40;
            b.add_element(node);
        }
        b.end();
    }
    b.end(string::dimension{ 400, 300 });

    const string::layout_node* p = b.find(string::make_id("panel").hash);
    const string::layout_node* n = b.find(string::make_id("node").hash);
    ASSERT_NE(p, nullptr);
    ASSERT_NE(n, nullptr);
    // Parent origin + its padding + the anchor.
    EXPECT_EQ(n->box.x, p->box.x + 10 + 30);
    EXPECT_EQ(n->box.y, p->box.y + 20 + 40);
}

TEST(LayoutTest, RootFloatingIsUnchangedByTheNewFlag)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(400, 300);
    b.begin(root, string::format{ .direction = string::direction::VERTICAL });
    {
        string::element panel{};
        panel.sizing = string::size_fixed(200, 150);
        b.begin(panel, string::format{ .padding = { 10, 0, 20, 0 } });
        {
            string::element node{};
            node.id = string::make_id("node");
            node.sizing = string::size_fixed(20, 20);
            node.floating = true;   // float_local left false
            node.float_x = 30;
            node.float_y = 40;
            b.add_element(node);
        }
        b.end();
    }
    b.end(string::dimension{ 400, 300 });

    const string::layout_node* n = b.find(string::make_id("node").hash);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(b.screen_box(*n).x, 30) << "root-space anchors must be untouched";
    EXPECT_EQ(b.screen_box(*n).y, 40);
}

// --- Signed positions (brief 13 follow-up) --------------------------------------------------------
// Content genuinely lives above and left of the origin: a scrolled view draws earlier rows above the
// viewport, a panned canvas draws earlier columns to the left. Unsigned coordinates made those
// positions unrepresentable, so such content could only be culled at the edge, never clipped.

TEST(LayoutTest, ANegativeAnchorIsRepresented)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(400, 300);
    b.begin(root, string::format{ .direction = string::direction::VERTICAL });
    {
        string::element n{};
        n.id = string::make_id("above");
        n.sizing = string::size_fixed(40, 40);
        n.floating = true;
        n.float_x = -30;
        n.float_y = -20;
        b.add_element(n);
    }
    b.end(string::dimension{ 400, 300 });

    const string::layout_node* n = b.find(string::make_id("above").hash);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(b.screen_box(*n).x, -30) << "a negative anchor must survive, not clamp to the edge";
    EXPECT_EQ(b.screen_box(*n).y, -20);
    // ...and it survives as a PLACEMENT: signedness has to hold in the surface, not just in the box.
    EXPECT_EQ(b.surfaces()[n->surface].placement.x, -30);
    EXPECT_EQ(b.surfaces()[n->surface].placement.y, -20);
}

// Hit-testing must not treat a negative position as enormous — the classic unsigned-underflow bug.
TEST(LayoutTest, HitTestingHandlesNegativePositions)
{
    string::bounding_box box{ -50, -20, { 100, 60 } };
    EXPECT_TRUE(box.contains(0, 0));
    EXPECT_TRUE(box.contains(-49, -19));
    EXPECT_FALSE(box.contains(-51, 0));
    EXPECT_FALSE(box.contains(60, 0));
}

// A child positioned locally inside a scrolled parent lands where the arithmetic says, including
// when that is off the parent's top-left.
TEST(LayoutTest, LocalAnchorsMayBeNegativeToo)
{
    string::layout_builder b;
    string::element root{};
    root.sizing = string::size_fixed(400, 300);
    b.begin(root, string::format{ .direction = string::direction::VERTICAL });
    {
        string::element panel{};
        panel.id = string::make_id("view");
        panel.sizing = string::size_fixed(200, 150);
        b.begin(panel, string::format{});
        {
            string::element row{};
            row.id = string::make_id("scrolled");
            row.sizing = string::size_fixed(180, 20);
            row.floating = true;
            row.float_local = true;
            row.float_y = -40;   // scrolled above the viewport
            b.add_element(row);
        }
        b.end();
    }
    b.end(string::dimension{ 400, 300 });

    const string::layout_node* v = b.find(string::make_id("view").hash);
    const string::layout_node* s = b.find(string::make_id("scrolled").hash);
    ASSERT_NE(v, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->box.y, v->box.y - 40);
}

// --- 12b M3: clean surfaces skip layout ----------------------------------------------------------
//
// The signature folds a surface's LAYOUT INPUTS and nothing else, so an unchanged surface restores
// last frame's boxes instead of re-running fit/flex/wrap/position. What must NOT enter the
// signature is the interesting half: paint (a Motion hover fade ticks a colour every frame — if
// that dirtied its panel the skip would degenerate to a full rebuild) and placement POSITION (M2's
// property — a dragged panel or a moving nameplate must stay clean).

namespace
{
// One surface — a floating "panel" with a couple of rows — parameterised by everything the tests
// want to vary. Authored into a FRESH frame on the same builder, so the retained store persists.
struct SurfaceSpec
{
    int x = 40;                       // placement position — must NOT dirty
    string::color fill{ 10, 20, 30, 255 };   // paint — must NOT dirty
    uint16_t radius = 4;              // paint — must NOT dirty
    uint16_t width = 200;             // sizing — MUST dirty
    std::string_view label = "row";   // text content — MUST dirty
    uint16_t gap = 6;                 // format — MUST dirty
    int scroll = 0;                   // a float_local anchor — MUST dirty
};

void author_surface(string::layout_builder& b, const SurfaceSpec& s,
                    string::dimension screen = { 800, 600 })
{
    b.clear();
    string::element root{};
    root.sizing = string::size_grow();
    b.begin(root, string::format{ .direction = string::direction::VERTICAL });
    {
        string::element panel{};
        panel.id = string::make_id("panel");
        panel.sizing = string::size_fixed(s.width, 120);
        panel.floating = true;          // root-space -> its own surface
        panel.float_x = static_cast<int16_t>(s.x);
        panel.float_y = 50;
        panel.color = s.fill;
        panel.radius = s.radius;
        b.begin(panel, string::format{ .gap = s.gap, .direction = string::direction::VERTICAL });
        {
            string::element row{};
            row.id = string::make_id("row");
            row.sizing = string::size_fixed(80, 20);
            row.floating = true;
            row.float_local = true;     // stays IN the surface; its anchor IS a layout input
            row.float_y = static_cast<int16_t>(s.scroll);
            b.add_element(row);
            b.add_text(string::element{}, s.label, 16);
        }
        b.end();
    }
    b.end(screen);
}
}  // namespace

TEST(LayoutTest, AnUnchangedSurfaceIsRestoredRatherThanLaidOut)
{
    string::layout_builder b;
    author_surface(b, {});
    EXPECT_EQ(b.skipped_count(), 0u) << "nothing can be clean on the first frame";
    EXPECT_GT(b.relayout_count(), 0u);

    author_surface(b, {});
    EXPECT_GT(b.skipped_count(), 0u) << "an identical frame re-laid-out everything";
}

// The restored boxes must be the boxes layout would have produced. A skip that returns the wrong
// geometry is far worse than no skip at all, so this compares against the same frame built with
// skipping forced off — which is exactly what STRING_UI_NO_SKIP buys at runtime.
TEST(LayoutTest, RestoredBoxesMatchAFullRelayout)
{
    string::layout_builder skipping;
    author_surface(skipping, {});
    author_surface(skipping, {});
    ASSERT_GT(skipping.skipped_count(), 0u) << "precondition: this frame actually skipped";

    string::layout_builder always;
    always.set_skip_enabled(false);
    author_surface(always, {});
    author_surface(always, {});
    ASSERT_EQ(always.skipped_count(), 0u) << "precondition: the control never skips";

    ASSERT_EQ(skipping.nodes().size(), always.nodes().size());
    for (std::size_t i = 0; i < always.nodes().size(); ++i)
    {
        const string::bounding_box a = skipping.screen_box(skipping.nodes()[i]);
        const string::bounding_box e = always.screen_box(always.nodes()[i]);
        EXPECT_EQ(a.x, e.x) << "node " << i;
        EXPECT_EQ(a.y, e.y) << "node " << i;
        EXPECT_EQ(a.dimension.width, e.dimension.width) << "node " << i;
        EXPECT_EQ(a.dimension.height, e.dimension.height) << "node " << i;
    }
}

// PAINT must not dirty. This is the input classification earning its keep: without it, one animated
// colour anywhere on a panel would relayout that panel every frame forever.
TEST(LayoutTest, AnimatingPaintForAHundredFramesCausesNoRelayout)
{
    string::layout_builder b;
    author_surface(b, {});

    std::size_t relayouts = 0;
    for (int f = 0; f < 100; ++f)
    {
        SurfaceSpec s{};
        // A hover fade and a radius tween — different values every single frame.
        s.fill = string::color{ static_cast<uint8_t>(f), static_cast<uint8_t>(255 - f), 40, 255 };
        s.radius = static_cast<uint16_t>(f % 12);
        author_surface(b, s);
        relayouts += b.relayout_count();
    }
    EXPECT_EQ(relayouts, 0u) << "paint entered the signature — the skip is defeated by animation";
}

// PLACEMENT POSITION must not dirty. M2 made this true of the boxes; M3 is what turns it into a
// saved relayout. A nameplate gliding across the screen is the case that pays for both.
TEST(LayoutTest, MovingASurfaceForAHundredFramesCausesNoRelayout)
{
    string::layout_builder b;
    author_surface(b, {});

    std::size_t relayouts = 0;
    for (int f = 0; f < 100; ++f)
    {
        SurfaceSpec s{};
        s.x = 40 + f * 3;
        author_surface(b, s);
        relayouts += b.relayout_count();
    }
    EXPECT_EQ(relayouts, 0u) << "position is still behaving as a layout input";

    // ...and it really did move: the skip must not be hiding a surface that stopped tracking.
    const string::layout_node* p = b.find(string::make_id("panel").hash);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(b.screen_box(*p).x, 40 + 99 * 3);
}

// Every LAYOUT input, one at a time: each must dirty, or the skip serves stale geometry.
TEST(LayoutTest, EachLayoutInputDirtiesItsSurface)
{
    const auto dirties = [](const SurfaceSpec& changed, const char* what) {
        string::layout_builder b;
        author_surface(b, {});
        author_surface(b, {});
        ASSERT_GT(b.skipped_count(), 0u) << "precondition failed for " << what;
        author_surface(b, changed);
        EXPECT_GT(b.relayout_count(), 0u) << what << " did not dirty its surface";
    };

    dirties({ .width = 260 }, "sizing");
    dirties({ .label = "a much longer row label" }, "text content");
    dirties({ .gap = 20 }, "format gap");
    dirties({ .scroll = -30 }, "a float_local anchor (scrolling is content, not placement)");

    // Resizing the viewport is a layout constraint, not a placement: it must reflow.
    string::layout_builder b;
    author_surface(b, {});
    author_surface(b, {});
    ASSERT_GT(b.skipped_count(), 0u);
    author_surface(b, {}, string::dimension{ 1024, 768 });
    EXPECT_GT(b.relayout_count(), 0u) << "a resize did not reflow";
}

// Structure, specifically: same nodes, different NESTING. Without the depth fold these two trees
// produce the same signature and the second silently reuses the first's boxes.
TEST(LayoutTest, ReNestingTheSameNodesDirtiesTheSurface)
{
    const auto author = [](string::layout_builder& b, bool nested) {
        b.clear();
        string::element root{};
        root.sizing = string::size_fixed(200, 200);
        b.begin(root, string::format{ .direction = string::direction::VERTICAL });
        string::element box{};
        box.sizing = string::size_fixed(40, 40);
        if (nested)
        {
            b.begin(box, string::format{});
            b.add_element(box);
            b.end();
        }
        else
        {
            b.add_element(box);
            b.add_element(box);
        }
        b.end(string::dimension{ 200, 200 });
    };

    string::layout_builder b;
    author(b, false);
    author(b, false);
    ASSERT_GT(b.skipped_count(), 0u) << "precondition: the flat tree is clean when repeated";

    author(b, true);
    EXPECT_GT(b.relayout_count(), 0u) << "re-nesting was invisible to the signature";
}
