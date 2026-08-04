#include <gtest/gtest.h>

#include <string/ui/ui.hpp>
#include <string/ui/workspace.hpp>

using namespace string;
using namespace string::ui;

namespace
{
// Cards are addressed by hash. In app code these come from `"stats"_id` constants; here plain
// numbers keep the tests about structure rather than hashing.
constexpr std::uint64_t A = 1, B = 2, C = 3, D = 4;

// Walks the tree collecting cards left-to-right, so a test can assert the ARRANGEMENT rather than
// poking at node indices.
void collect(const Workspace& ws, node_index n, std::vector<std::uint64_t>& out)
{
    if (!ws.valid(n)) return;
    const workspace_node& node = ws.node(n);
    if (node.is_panel())
    {
        out.insert(out.end(), node.cards.begin(), node.cards.end());
        return;
    }
    collect(ws, node.a, out);
    collect(ws, node.b, out);
}

std::vector<std::uint64_t> docked_order(const Workspace& ws)
{
    std::vector<std::uint64_t> out;
    collect(ws, ws.root(), out);
    return out;
}

// Every live node's parent link must agree with its parent's child links, and roots must have none.
// Structural corruption is the failure mode that makes dock trees miserable to debug, so it gets
// checked after every mutation rather than trusted.
void check_links(const Workspace& ws)
{
    if (ws.root() != no_node)
        EXPECT_EQ(ws.node(ws.root()).parent, no_node) << "docked root must have no parent";
    for (node_index f : ws.floating())
        EXPECT_EQ(ws.node(f).parent, no_node) << "floating root must have no parent";

    for (node_index i = 0; i < 64; ++i)
    {
        if (!ws.valid(i)) continue;
        const workspace_node& n = ws.node(i);
        if (!n.is_split()) continue;
        ASSERT_TRUE(ws.valid(n.a)) << "split " << i << " has a dead child a";
        ASSERT_TRUE(ws.valid(n.b)) << "split " << i << " has a dead child b";
        EXPECT_EQ(ws.node(n.a).parent, i);
        EXPECT_EQ(ws.node(n.b).parent, i);
        EXPECT_NE(n.a, n.b);
    }
}
}  // namespace

// --- Docking -------------------------------------------------------------------------------------

TEST(WorkspaceTest, FirstCardBecomesTheRootPanel)
{
    Workspace ws;
    EXPECT_TRUE(ws.empty());
    ASSERT_TRUE(ws.dock(A, left(fixed(280))));
    EXPECT_FALSE(ws.empty());
    ASSERT_NE(ws.root(), no_node);
    EXPECT_TRUE(ws.node(ws.root()).is_panel());
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A }));
    check_links(ws);
}

TEST(WorkspaceTest, SideSplitsPutTheNewCardOnTheRequestedSide)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, left(fixed(280)));   // B to the LEFT of the whole arrangement
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ B, A }));

    Workspace ws2;
    ws2.dock(A, left());
    ws2.dock(B, right(fixed(280)));
    EXPECT_EQ(docked_order(ws2), (std::vector<std::uint64_t>{ A, B }));
    check_links(ws);
    check_links(ws2);
}

TEST(WorkspaceTest, SideChoosesTheSplitAxis)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, bottom(fixed(200)));
    ASSERT_TRUE(ws.node(ws.root()).is_split());
    EXPECT_EQ(ws.node(ws.root()).dir, direction::VERTICAL);

    Workspace ws2;
    ws2.dock(A, left());
    ws2.dock(B, right(fixed(200)));
    EXPECT_EQ(ws2.node(ws2.root()).dir, direction::HORIZONTAL);
}

// The new region carries the requested size; the existing one takes what is left. This is what
// makes "a 280px strip beside everything else" mean what an author expects.
TEST(WorkspaceTest, NewRegionCarriesTheSizingAndTheOldOneGrows)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, left(fixed(280)));

    const workspace_node& root = ws.node(ws.root());
    const workspace_node& newer = ws.node(root.a);   // B is leading
    const workspace_node& older = ws.node(root.b);
    EXPECT_EQ(newer.sizing.mode, size_mode::FIXED);
    EXPECT_EQ(newer.sizing.value, 280);
    EXPECT_EQ(older.sizing.mode, size_mode::GROW);
}

TEST(WorkspaceTest, LockedIsCarriedOntoTheSplit)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, left(fixed(280)).locked());
    EXPECT_TRUE(ws.node(ws.root()).locked);

    Workspace ws2;
    ws2.dock(A, left());
    ws2.dock(B, left(fixed(280)));
    EXPECT_FALSE(ws2.node(ws2.root()).locked);
}

TEST(WorkspaceTest, CardRelativePlacementSplitsThatCardsPanel)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right(grow_weighted(1)));
    ws.dock(C, bottom_of(A, fixed(120)));   // under A specifically, not under everything

    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A, C, B }));
    const node_index pc = ws.panel_of(C);
    ASSERT_NE(pc, no_node);
    EXPECT_EQ(ws.node(ws.node(pc).parent).dir, direction::VERTICAL);
    check_links(ws);
}

TEST(WorkspaceTest, PlacementAgainstAMissingAnchorFails)
{
    Workspace ws;
    ws.dock(A, left());
    EXPECT_FALSE(ws.dock(B, right_of(D)));     // D was never docked
    EXPECT_FALSE(ws.contains(B));
    EXPECT_FALSE(ws.dock(B, tab_with(D)));
    EXPECT_FALSE(ws.contains(B));
}

// --- Tabs ----------------------------------------------------------------------------------------

TEST(WorkspaceTest, TabWithJoinsTheAnchorsPanelAndSelectsTheNewCard)
{
    Workspace ws;
    ws.dock(A, left());
    ASSERT_TRUE(ws.dock(B, tab_with(A)));
    const node_index p = ws.panel_of(A);
    EXPECT_EQ(p, ws.panel_of(B));
    EXPECT_EQ(ws.node(p).cards.size(), 2u);
    EXPECT_EQ(ws.node(p).selected, 1u);   // the newly docked card is shown
    check_links(ws);
}

TEST(WorkspaceTest, SelectPicksTheVisibleTab)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, tab_with(A));
    ws.dock(C, tab_with(A));
    ASSERT_TRUE(ws.select(A));
    EXPECT_EQ(ws.node(ws.panel_of(A)).selected, 0u);
    EXPECT_FALSE(ws.select(D));
}

// Removing a tab before the visible one must keep the SAME card visible, not shift to a neighbour.
TEST(WorkspaceTest, ClosingAnEarlierTabKeepsTheSameCardVisible)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, tab_with(A));
    ws.dock(C, tab_with(A));
    ws.select(C);
    const node_index p = ws.panel_of(A);
    ASSERT_EQ(ws.node(p).selected, 2u);

    ws.close(A);
    EXPECT_EQ(ws.node(p).cards.size(), 2u);
    EXPECT_EQ(ws.node(p).cards[ws.node(p).selected], C);
}

TEST(WorkspaceTest, ClosingTheLastTabClampsTheSelection)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, tab_with(A));
    ws.select(B);
    ws.close(B);
    const node_index p = ws.panel_of(A);
    EXPECT_EQ(ws.node(p).selected, 0u);
}

// --- Collapse (the part that gets dock implementations wrong) ------------------------------------

TEST(WorkspaceTest, ClosingTheLastCardOfAPanelCollapsesTheSplit)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right(fixed(280)));
    ASSERT_TRUE(ws.node(ws.root()).is_split());

    ws.close(B);
    // The split AND the empty panel are gone; A's panel is the root again.
    EXPECT_TRUE(ws.node(ws.root()).is_panel());
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A }));
    EXPECT_EQ(ws.node_count(), 1u);
    check_links(ws);
}

// THE precise version of "the ratios merge": the split occupied a region of a given size within its
// parent, and the survivor must now occupy exactly that region. Getting this wrong makes a layout
// visibly jump when an unrelated card is closed.
TEST(WorkspaceTest, SurvivorInheritsTheCollapsedSplitsSizing)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right(fixed(300)));     // root split: [A grow | B 300]
    ws.dock(C, bottom_of(B, fixed(120)));

    // B's panel is now inside a nested split that occupies B's old 300px region.
    const node_index inner = ws.node(ws.panel_of(B)).parent;
    ASSERT_TRUE(ws.valid(inner));
    EXPECT_EQ(ws.node(inner).sizing.mode, size_mode::FIXED);
    EXPECT_EQ(ws.node(inner).sizing.value, 300);

    ws.close(C);
    // The inner split collapsed; B's panel must have taken over its 300px region.
    const node_index pb = ws.panel_of(B);
    EXPECT_EQ(ws.node(pb).sizing.mode, size_mode::FIXED);
    EXPECT_EQ(ws.node(pb).sizing.value, 300);
    check_links(ws);
}

TEST(WorkspaceTest, ClosingEverythingLeavesAnEmptyWorkspace)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    ws.dock(C, bottom());
    ws.close(A);
    ws.close(B);
    ws.close(C);
    EXPECT_TRUE(ws.empty());
    EXPECT_EQ(ws.root(), no_node);
    EXPECT_EQ(ws.node_count(), 0u);
}

TEST(WorkspaceTest, NestedCollapsesUnwindCorrectly)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    ws.dock(C, bottom_of(B));
    ws.dock(D, right_of(C));
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A, B, C, D }));

    ws.close(D);
    check_links(ws);
    ws.close(C);
    check_links(ws);
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A, B }));
    EXPECT_EQ(ws.node_count(), 3u);   // one split, two panels
}

// --- Moves (tear-off, drag-to-dock and undock are the same operation) ----------------------------

TEST(WorkspaceTest, DockingAnExistingCardMovesItRatherThanDuplicating)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    ws.dock(C, tab_with(B));

    ASSERT_TRUE(ws.dock(C, tab_with(A)));
    EXPECT_EQ(ws.panel_of(C), ws.panel_of(A));
    EXPECT_NE(ws.panel_of(C), ws.panel_of(B));
    // Exactly one copy: B's panel is back to holding only B.
    EXPECT_EQ(ws.node(ws.panel_of(B)).cards.size(), 1u);
    check_links(ws);
}

TEST(WorkspaceTest, MovingACardRelativeToItselfIsRejected)
{
    Workspace ws;
    ws.dock(A, left());
    // A is alone in its panel — "beside itself" has no meaning, and must not corrupt the tree.
    EXPECT_FALSE(ws.dock(A, right_of(A)));
    EXPECT_FALSE(ws.dock(A, tab_with(A)));
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A }));
    check_links(ws);
}

TEST(WorkspaceTest, UndockMovesACardToItsOwnFloatingPanel)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, tab_with(A));

    ASSERT_TRUE(ws.undock(B));
    EXPECT_EQ(ws.floating().size(), 1u);
    EXPECT_EQ(ws.node(ws.floating()[0]).cards, (std::vector<std::uint64_t>{ B }));
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A }));
    check_links(ws);
}

// Tear-off of the only docked card: the docked tree empties and the card lives on, floating.
TEST(WorkspaceTest, UndockingTheLastDockedCardEmptiesTheDockedTree)
{
    Workspace ws;
    ws.dock(A, left());
    ASSERT_TRUE(ws.undock(A));
    EXPECT_EQ(ws.root(), no_node);
    EXPECT_EQ(ws.floating().size(), 1u);
    EXPECT_TRUE(ws.contains(A));
    EXPECT_FALSE(ws.empty());
    check_links(ws);
}

// Dropping a floating card back onto the docked tree — the same `dock` call, no conversion path.
TEST(WorkspaceTest, AFloatingCardCanBeDockedBackIn)
{
    Workspace ws;
    ws.dock(A, left());
    ws.undock(A);
    ws.dock(B, left());
    ASSERT_TRUE(ws.dock(A, right_of(B)));
    EXPECT_TRUE(ws.floating().empty());   // the emptied floating panel was deleted outright
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ B, A }));
    check_links(ws);
}

TEST(WorkspaceTest, ClosingAFloatingCardRemovesItsPanel)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, floating_at({ 10, 10, 200, 150 }));
    ASSERT_EQ(ws.floating().size(), 1u);
    ws.close(B);
    EXPECT_TRUE(ws.floating().empty());
    EXPECT_FALSE(ws.contains(B));
    check_links(ws);
}

// --- Floating order ------------------------------------------------------------------------------

TEST(WorkspaceTest, FloatingPanelsAreBackToFrontWithNewestOnTop)
{
    Workspace ws;
    ws.dock(A, floating_at({}));
    ws.dock(B, floating_at({}));
    ASSERT_EQ(ws.floating().size(), 2u);
    EXPECT_EQ(ws.node(ws.floating()[1]).cards[0], B);
}

TEST(WorkspaceTest, BringToFrontRaisesWithoutDisturbingTheRest)
{
    Workspace ws;
    ws.dock(A, floating_at({}));
    ws.dock(B, floating_at({}));
    ws.dock(C, floating_at({}));
    const node_index first = ws.floating()[0];
    ws.bring_to_front(first);
    EXPECT_EQ(ws.floating().back(), first);
    EXPECT_EQ(ws.node(ws.floating()[0]).cards[0], B);
    EXPECT_EQ(ws.node(ws.floating()[1]).cards[0], C);
}

// --- Storage -------------------------------------------------------------------------------------

// Recycled slots must not resurrect stale cards — a freed node handed back out with its old
// contents would silently duplicate a card.
TEST(WorkspaceTest, RecycledNodesDoNotResurrectOldCards)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    ws.close(B);
    ws.dock(C, right());
    EXPECT_FALSE(ws.contains(B));
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ A, C }));
    check_links(ws);
}

TEST(WorkspaceTest, ClearResetsEverything)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, floating_at({}));
    ws.clear();
    EXPECT_TRUE(ws.empty());
    EXPECT_EQ(ws.node_count(), 0u);
    EXPECT_FALSE(ws.contains(A));
}

// --- Splitter drag (pure; capture-at-press is the caller's job) ----------------------------------

namespace
{
// Builds `A | B` with the given sizings and returns the split node.
//
// NOTE the first dock's sizing is deliberately NOT used: the first card becomes the ROOT, which has
// no parent split, so a sizing on it would have nothing to size against. A's sizing is therefore set
// explicitly after the structure exists.
node_index two_up(Workspace& ws, axis_sizing sa, axis_sizing sb, bool lock = false)
{
    ws.dock(A, left());
    placement p = right(sb);
    if (lock) p = p.locked();
    ws.dock(B, p);
    const node_index split = ws.node(ws.panel_of(A)).parent;
    // BOTH set explicitly: split_node now pairs non-definite placements as percent, so leaving one
    // side implicit would silently test a mixed pair rather than the branch each test names.
    ws.node(ws.panel_of(A)).sizing = sa;
    ws.node(ws.panel_of(B)).sizing = sb;
    return split;
}
}  // namespace

// A fixed strip stays fixed: dragging changes its pixel size rather than silently converting it to
// a proportion. The author's intent survives the gesture.
TEST(WorkspaceTest, DraggingAFixedRegionAdjustsItsPixels)
{
    Workspace ws;
    const node_index s = two_up(ws, fixed(200), grow());
    ws.drag_splitter(s, 40.0f, 200, 600);
    const workspace_node& a = ws.node(ws.panel_of(A));
    EXPECT_EQ(a.sizing.mode, size_mode::FIXED);
    EXPECT_EQ(a.sizing.value, 240);
}

TEST(WorkspaceTest, DraggingProportionalRegionsRescalesWeights)
{
    Workspace ws;
    const node_index s = two_up(ws, grow_weighted(5), grow_weighted(5));
    ws.drag_splitter(s, 200.0f, 500, 500);   // boundary to 700/300
    const workspace_node& a = ws.node(ws.panel_of(A));
    const workspace_node& b = ws.node(ws.panel_of(B));
    EXPECT_EQ(a.sizing.mode, size_mode::GROW);
    EXPECT_GT(a.sizing.weight, b.sizing.weight);
    // Renormalised to a fixed total, so the sum is stable AND has resolution to spare.
    EXPECT_EQ(a.sizing.weight + b.sizing.weight, 1000);
}

// Repeated drags must not drift the weight sum — otherwise a few dozen drags silently degrade the
// precision of every future one.
TEST(WorkspaceTest, RepeatedProportionalDragsDoNotDriftTheWeightSum)
{
    Workspace ws;
    const node_index s = two_up(ws, grow_weighted(50), grow_weighted(50));
    for (int i = 0; i < 20; ++i)
        ws.drag_splitter(s, (i % 2 == 0) ? 30.0f : -30.0f, 500, 500);
    const workspace_node& a = ws.node(ws.panel_of(A));
    const workspace_node& b = ws.node(ws.panel_of(B));
    EXPECT_EQ(a.sizing.weight + b.sizing.weight, 1000);
}

TEST(WorkspaceTest, DragIsClampedSoNeitherSideVanishes)
{
    Workspace ws;
    const node_index s = two_up(ws, fixed(200), grow());
    ws.drag_splitter(s, -5000.0f, 200, 600);
    EXPECT_EQ(ws.node(ws.panel_of(A)).sizing.value, ws.min_region());

    Workspace ws2;
    const node_index s2 = two_up(ws2, fixed(200), grow());
    ws2.drag_splitter(s2, 5000.0f, 200, 600);
    EXPECT_EQ(ws2.node(ws2.panel_of(A)).sizing.value, 800 - ws2.min_region());
}

TEST(WorkspaceTest, ALockedSplitterIgnoresDrags)
{
    Workspace ws;
    const node_index s = two_up(ws, grow(), fixed(200), /*lock=*/true);
    ASSERT_TRUE(ws.node(s).locked);
    ws.drag_splitter(s, 100.0f, 600, 200);
    EXPECT_EQ(ws.node(ws.panel_of(B)).sizing.value, 200);
}

// Dragging is absolute-from-press (the caller passes the ORIGIN sizes), so re-applying a larger
// delta must not accumulate on top of the previous result.
TEST(WorkspaceTest, DragIsAbsoluteFromTheCapturedOrigin)
{
    Workspace ws;
    const node_index s = two_up(ws, fixed(200), grow());
    ws.drag_splitter(s, 40.0f, 200, 600);
    ws.drag_splitter(s, 90.0f, 200, 600);
    EXPECT_EQ(ws.node(ws.panel_of(A)).sizing.value, 290);   // not 200 + 40 + 90
}

// --- Emit + observe + drag, end to end ------------------------------------------------------------
// The pure drag maths is covered above; this exercises the whole path the app uses, which is where a
// break would actually live: emit sets element hashes, layout resolves sizes, observe caches them,
// and only then can a drag convert pixels into sizing.

#include <string/ui/ui.hpp>

namespace
{
struct WsRig
{
    layout_builder builder;
    interaction state{};
    Motion motion;
    Theme theme{};
    PanelStore panels;
    Ui ui{ builder, state, motion, theme, panels };
    Workspace ws;

    // One frame: author the workspace into a screen-filling root, lay out, then observe.
    void frame(dimension screen = { 800, 600 })
    {
        state.screen = screen;
        ui.begin_frame();
        builder.clear();
        builder.begin(format{ .direction = direction::VERTICAL });
        ui.workspace(ws).content([&](Ui& u) {
            u.card(A).title("A").content([](Ui& c) { c.text("a"); });
            u.card(B).title("B").content([](Ui& c) { c.text("b"); });
        });
        builder.end(screen);
        ws.observe(builder);
    }
};
}  // namespace

TEST(WorkspaceTest, EmitProducesAddressableNodesAndObserveRecordsTheirSizes)
{
    WsRig r;
    r.ws.dock(A, left());
    r.ws.dock(B, right(fixed(200)));
    r.frame();

    const node_index split = r.ws.node(r.ws.panel_of(A)).parent;
    ASSERT_TRUE(r.ws.valid(split));
    // Every emitted node must be findable, or observe() silently caches nothing and drag is a no-op.
    EXPECT_NE(r.ws.node(split).element_hash, 0u);
    EXPECT_NE(r.ws.node(r.ws.panel_of(A)).element_hash, 0u);
    EXPECT_NE(r.ws.node(r.ws.panel_of(B)).element_hash, 0u);

    EXPECT_GT(r.ws.node(r.ws.panel_of(A)).resolved, 0);
    EXPECT_EQ(r.ws.node(r.ws.panel_of(B)).resolved, 200);
}

// THE path a user exercises: press the splitter, move, and the region resizes.
TEST(WorkspaceTest, DraggingTheEmittedSplitterResizesTheRegion)
{
    WsRig r;
    r.ws.dock(A, left());
    r.ws.dock(B, right(fixed(200)));
    r.frame();

    const node_index split = r.ws.node(r.ws.panel_of(A)).parent;
    const std::uint64_t grip = make_id("ws.grip." + std::to_string(r.ws.node(split).uid)).hash;

    // The splitter must exist in the tree under exactly the id the emit code derives.
    ASSERT_NE(r.builder.find(grip), nullptr) << "splitter element not found by its derived id";

    // Simulate a held drag on it, then re-author.
    r.state.active = grip;
    r.state.hovered = grip;
    r.state.drag_x = -60.0f;
    r.frame();

    EXPECT_EQ(r.ws.node(r.ws.panel_of(B)).sizing.value, 260) << "B should have grown by the drag";
}

// REGRESSION: the default weights are 1 and 1. An earlier version renormalised to the children's
// EXISTING sum, so the sum was 2, the clamp range collapsed to [1,1], and the weights could never
// change — the splitter was frozen for every default proportional split. The tests that existed at
// the time used 5/5 and 50/50, which have headroom, and missed it entirely.
TEST(WorkspaceTest, DefaultUnitWeightsCanStillBeDragged)
{
    Workspace ws;
    const node_index s = two_up(ws, grow(), grow());
    ASSERT_EQ(ws.node(ws.panel_of(A)).sizing.weight, 1);
    ASSERT_EQ(ws.node(ws.panel_of(B)).sizing.weight, 1);

    ws.drag_splitter(s, 200.0f, 400, 400);
    const workspace_node& a = ws.node(ws.panel_of(A));
    const workspace_node& b = ws.node(ws.panel_of(B));
    EXPECT_GT(a.sizing.weight, b.sizing.weight) << "unit weights must still be draggable";
    EXPECT_NEAR(a.sizing.weight, 750, 2);   // 600/800 of the total
}

TEST(WorkspaceTest, DraggingAPercentPairRewritesThePerMilleShares)
{
    Workspace ws;
    const node_index s = two_up(ws, percent(500), percent(500));
    ws.drag_splitter(s, 200.0f, 400, 400);
    const workspace_node& a = ws.node(ws.panel_of(A));
    const workspace_node& b = ws.node(ws.panel_of(B));
    EXPECT_EQ(a.sizing.mode, size_mode::PERCENT);
    EXPECT_EQ(a.sizing.value, 750);            // 600 of 800
    EXPECT_EQ(a.sizing.value + b.sizing.value, 1000);
}

// A non-definite placement must produce a PERCENT pair, not GROW siblings — this is what makes the
// splitter track the cursor rather than jump.
TEST(WorkspaceTest, NonDefiniteSplitsArePercentPairs)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    EXPECT_EQ(ws.node(ws.panel_of(A)).sizing.mode, size_mode::PERCENT);
    EXPECT_EQ(ws.node(ws.panel_of(B)).sizing.mode, size_mode::PERCENT);
    EXPECT_EQ(ws.node(ws.panel_of(A)).sizing.value + ws.node(ws.panel_of(B)).sizing.value, 1000);

    // A FIXED placement is left alone: a fixed strip beside a growing remainder already round-trips.
    Workspace ws2;
    ws2.dock(A, left());
    ws2.dock(B, right(fixed(200)));
    EXPECT_EQ(ws2.node(ws2.panel_of(B)).sizing.mode, size_mode::FIXED);
    EXPECT_EQ(ws2.node(ws2.panel_of(A)).sizing.mode, size_mode::GROW);
}

// THE regression for "clicking the splitter repositions it": a press with no movement must be a
// no-op. It was not, because GROW weights derived from resolved pixels do not round-trip — the
// layout re-applied them on top of content sizes and the boundary moved.
TEST(WorkspaceTest, ClickingASplitterWithoutMovingItChangesNothing)
{
    WsRig r;
    r.ws.dock(A, left());
    r.ws.dock(B, right());
    r.frame();

    const node_index split = r.ws.node(r.ws.panel_of(A)).parent;
    const std::uint16_t before_a = r.ws.node(r.ws.panel_of(A)).resolved;
    const std::uint16_t before_b = r.ws.node(r.ws.panel_of(B)).resolved;
    ASSERT_GT(before_a, 0);

    // Press, no movement.
    r.state.active = make_id("ws.grip." + std::to_string(r.ws.node(split).uid)).hash;
    r.state.hovered = r.state.active;
    r.state.drag_x = 0.0f;
    r.frame();

    EXPECT_EQ(r.ws.node(r.ws.panel_of(A)).resolved, before_a);
    EXPECT_EQ(r.ws.node(r.ws.panel_of(B)).resolved, before_b);
}

// --- The minimum PERCENT does not give for free ---------------------------------------------------
// GROW started a child at its CONTENT size, so a region could never be dragged below what it holds.
// PERCENT ignores content — the very property that makes the splitter round-trip — so the floor has
// to be explicit. Without it the region shrinks past its own chrome, the layout's overflow pass
// squeezes the text rows toward zero, and the glyphs clip into garbage.

TEST(WorkspaceTest, NeitherSideCanBeDraggedBelowTheMinimumRegion)
{
    Workspace ws;
    const node_index s = two_up(ws, percent(500), percent(500));
    const int total = 800;

    ws.drag_splitter(s, -5000.0f, 400, 400);
    const int pa = ws.node(ws.panel_of(A)).sizing.value;
    EXPECT_GE(pa * total / 1000, ws.min_region() - 1);

    ws.drag_splitter(s, 5000.0f, 400, 400);
    const int pb = ws.node(ws.panel_of(B)).sizing.value;
    EXPECT_GE(pb * total / 1000, ws.min_region() - 1);
}

// The floor is on the SIZING too, not just the drag: a window resize can squeeze a region without
// any drag being involved.
TEST(WorkspaceTest, SplitChildrenCarryTheMinimumInTheirSizing)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    EXPECT_EQ(ws.node(ws.panel_of(A)).sizing.min, ws.min_region());
    EXPECT_EQ(ws.node(ws.panel_of(B)).sizing.min, ws.min_region());
}

TEST(WorkspaceTest, MinimumRegionIsConfigurable)
{
    Workspace ws;
    ws.set_min_region(200);
    ws.dock(A, left());
    ws.dock(B, right());
    EXPECT_EQ(ws.node(ws.panel_of(B)).sizing.min, 200);
}

// --- Drag to dock (M2d) ---------------------------------------------------------------------------
// Tear-off, drag-to-dock and reorder are ONE gesture. These drive it directly: pick a card up,
// resolve a target from the boxes observe() cached, drop.

namespace
{
// Gives the panels boxes without running a layout, so the zone maths can be tested in isolation.
void fake_boxes(Workspace& ws, node_index n, panel_rect r)
{
    if (!ws.valid(n)) return;
    ws.node(n).box = r;
    if (!ws.node(n).is_split()) return;
    // Halve along the split axis; good enough for hit-testing tests.
    panel_rect a = r, b = r;
    if (ws.node(n).dir == direction::HORIZONTAL) { a.width = r.width / 2; b.x = r.x + a.width; b.width = r.width / 2; }
    else                                          { a.height = r.height / 2; b.y = r.y + a.height; b.height = r.height / 2; }
    fake_boxes(ws, ws.node(n).a, a);
    fake_boxes(ws, ws.node(n).b, b);
}
}  // namespace

TEST(WorkspaceTest, DropInTheMiddleOfAPanelJoinsItsTabs)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    fake_boxes(ws, ws.root(), { 0, 0, 800, 600 });

    ws.begin_card_drag(A, 0xAAAA);
    ws.update_card_drag(600.0f, 300.0f);   // middle of B's half
    EXPECT_TRUE(ws.dragging_card().as_tab);
    ASSERT_TRUE(ws.end_card_drag({}));
    EXPECT_EQ(ws.panel_of(A), ws.panel_of(B));
}

TEST(WorkspaceTest, DropNearAnEdgeSplitsOnThatSide)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    fake_boxes(ws, ws.root(), { 0, 0, 800, 600 });

    ws.begin_card_drag(A, 0xAAAA);
    ws.update_card_drag(600.0f, 580.0f);   // bottom edge of B's half
    EXPECT_FALSE(ws.dragging_card().as_tab);
    EXPECT_EQ(ws.dragging_card().zone, side::Bottom);
    ASSERT_TRUE(ws.end_card_drag({}));
    // A now sits under B, in B's region.
    EXPECT_EQ(docked_order(ws), (std::vector<std::uint64_t>{ B, A }));
    check_links(ws);
}

// A drop over nothing floats the card — which is what makes tear-off just another drop rather than
// a separate code path.
TEST(WorkspaceTest, DropOverNothingFloatsTheCard)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, tab_with(A));
    fake_boxes(ws, ws.root(), { 0, 0, 800, 600 });

    ws.begin_card_drag(B, 0xBBBB);
    ws.update_card_drag(5000.0f, 5000.0f);   // outside every panel
    EXPECT_EQ(ws.dragging_card().target, no_node);
    ASSERT_TRUE(ws.end_card_drag({ 40, 40, 300, 200 }));
    EXPECT_EQ(ws.floating().size(), 1u);
    EXPECT_EQ(ws.node(ws.floating()[0]).cards, (std::vector<std::uint64_t>{ B }));
    check_links(ws);
}

TEST(WorkspaceTest, DroppingACardOnItsOwnPanelDoesNothing)
{
    Workspace ws;
    ws.dock(A, left());
    fake_boxes(ws, ws.root(), { 0, 0, 800, 600 });
    const std::size_t before = ws.node_count();

    ws.begin_card_drag(A, 0xAAAA);
    ws.update_card_drag(400.0f, 300.0f);
    EXPECT_FALSE(ws.end_card_drag({}));
    EXPECT_EQ(ws.node_count(), before);
    check_links(ws);
}

TEST(WorkspaceTest, CancellingADragLeavesTheArrangementAlone)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    fake_boxes(ws, ws.root(), { 0, 0, 800, 600 });
    const auto before = docked_order(ws);

    ws.begin_card_drag(A, 0xAAAA);
    ws.update_card_drag(600.0f, 300.0f);
    ws.cancel_card_drag();
    EXPECT_FALSE(ws.dragging_card().active);
    EXPECT_EQ(docked_order(ws), before);
}

TEST(WorkspaceTest, DraggingAnAbsentCardIsRejected)
{
    Workspace ws;
    ws.dock(A, left());
    ws.begin_card_drag(D, 0xDDDD);   // never docked
    EXPECT_FALSE(ws.dragging_card().active);
}

// A floating panel can be dropped back into the docked tree — the same gesture, no conversion path.
TEST(WorkspaceTest, AFloatingCardCanBeDroppedBackIntoTheTree)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, floating_at({ 10, 10, 200, 150 }));
    fake_boxes(ws, ws.root(), { 0, 0, 800, 600 });
    ASSERT_EQ(ws.floating().size(), 1u);

    ws.begin_card_drag(B, 0xBBBB);
    ws.update_card_drag(400.0f, 300.0f);   // middle of A's panel
    ASSERT_TRUE(ws.end_card_drag({}));
    EXPECT_TRUE(ws.floating().empty()) << "the emptied floating panel must be deleted";
    EXPECT_EQ(ws.panel_of(A), ws.panel_of(B));
    check_links(ws);
}

// --- Subtree minimums (the three-way-split overlap) -----------------------------------------------

// A split's floor is what its whole SUBTREE needs, not one panel's worth. With a flat per-panel
// minimum the outer splitter of a three-way could be dragged until the inner pair no longer fitted,
// at which point they overflowed their container and visually overlapped the neighbouring region.
TEST(WorkspaceTest, ASplitsMinimumAccountsForItsWholeSubtree)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    ws.dock(C, bottom_of(B));   // B and C now share a vertical split on the right

    const node_index root = ws.root();
    const node_index inner = ws.node(ws.panel_of(B)).parent;
    ASSERT_TRUE(ws.valid(inner));
    ASSERT_TRUE(ws.node(inner).is_split());

    // Along the inner split's own axis its children stack, so the minimums ADD.
    EXPECT_EQ(ws.effective_min(inner, direction::VERTICAL),
              2 * ws.min_region() + ws.splitter_px());
    // ACROSS that axis they sit side by side, so the larger governs — not the sum.
    EXPECT_EQ(ws.effective_min(inner, direction::HORIZONTAL), ws.min_region());
    // The root is horizontal: a panel plus the inner split plus a splitter.
    EXPECT_EQ(ws.effective_min(root, direction::HORIZONTAL),
              2 * ws.min_region() + ws.splitter_px());
}

TEST(WorkspaceTest, TheOuterSplitterCannotCrushANestedPair)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());
    ws.dock(C, bottom_of(B));

    const node_index root = ws.root();
    const node_index inner = ws.node(ws.panel_of(B)).parent;
    const int total = 800;

    // Drag the outer splitter hard right, trying to squash the nested pair to nothing.
    ws.drag_splitter(root, 5000.0f, 400, 400);
    const int pb = ws.node(inner).sizing.value;   // per-mille left to the nested side
    const int px_b = pb * total / 1000;
    EXPECT_GE(px_b, ws.effective_min(inner, direction::HORIZONTAL))
        << "the nested split must keep room for its own subtree";
}

// A floating placement must carry its rect through. Dropping it left every torn-off panel at 0x0,
// which also made it unhittable and therefore unmovable.
TEST(WorkspaceTest, AFloatingPlacementKeepsItsRect)
{
    Workspace ws;
    ws.dock(A, floating_at({ 40, 50, 300, 220 }));
    ASSERT_EQ(ws.floating().size(), 1u);
    const panel_state& st = ws.node(ws.floating()[0]).floating_state;
    EXPECT_TRUE(st.placed);
    EXPECT_FLOAT_EQ(st.rect.width, 300.0f);
    EXPECT_FLOAT_EQ(st.rect.height, 220.0f);
    EXPECT_FLOAT_EQ(st.rect.x, 40.0f);
}

TEST(WorkspaceTest, TearingOffGivesTheNewPanelAUsableRect)
{
    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, tab_with(A));
    ws.begin_card_drag(B, 0xBBBB);
    ws.update_card_drag(5000.0f, 5000.0f);
    ASSERT_TRUE(ws.end_card_drag({ 100, 100, 320, 240 }));

    const panel_state& st = ws.node(ws.floating()[0]).floating_state;
    EXPECT_GT(st.rect.width, 0.0f);
    EXPECT_GT(st.rect.height, 0.0f);
}

// --- The drop preview's layer (brief 12b M1) -----------------------------------------------------
//
// The preview only exists mid-gesture, so eyeballing it means staging a drag-to-dock by hand. The
// drag state is plain data (`begin_card_drag` + `update_card_drag`), so the tree it produces can be
// asserted directly — which is a better proof than a screenshot anyway, and it does not evaporate.
//
// It used to be `overlay` + a hand-picked `z=253`, chosen to sit "above every panel, below the debug
// surfaces" — a layer that stopped existing when the debug surfaces moved onto the panel stack. The
// number was therefore arbitrary, and this pins what replaced it.
TEST(WorkspaceTest, DropPreviewDrawsOnThePopupLayerAbovePanels)
{
    layout_builder builder;
    interaction state{};
    state.screen = dimension{ 800, 600 };
    Motion motion;
    Theme theme{};
    PanelStore panels;
    Ui ui{ builder, state, motion, theme, panels };

    Workspace ws;
    ws.dock(A, left());
    ws.dock(B, right());

    // One frame to lay the workspace out, so `observe()` has boxes for the drag to resolve against.
    const auto author = [&] {
        ui.begin_frame();
        builder.clear();
        builder.begin(format{ .direction = direction::VERTICAL });
        ui.workspace(ws).content([&](Ui& u) {
            u.card(A).title("A").content([](Ui&) {});
            u.card(B).title("B").content([](Ui&) {});
        });
        builder.end(dimension{ 800, 600 });
        ws.observe(builder);
        ui.end_frame();
    };
    author();

    // Carry a card over the right-hand region. The workspace SUSTAINS a card drag only while its
    // element still holds pointer capture — otherwise the next authored frame treats it as a drop —
    // so the test drives the real path: hold capture, put the cursor somewhere, author.
    constexpr std::uint64_t kTabElement = 1234;
    state.active = kTabElement;
    state.cursor_x = 600.0f;
    state.cursor_y = 300.0f;
    ws.begin_card_drag(A, kTabElement);
    author();
    ASSERT_TRUE(ws.dragging_card().active) << "the drag was dropped instead of carried";
    ASSERT_NE(ws.dragging_card().target, no_node) << "no region resolved under the cursor";

    // Find it: the preview is the only POPUP-layer node in this tree.
    const layout_node* preview = nullptr;
    for (const layout_node& n : builder.nodes())
        if (n.element.layer == ui_layer::popups) preview = &n;
    if (preview == nullptr)
    {
        const auto& d = ws.dragging_card();
        const panel_rect& tb = ws.node(d.target).box;
        printf("DIAG target=%u valid=%d box=%.1f,%.1f %.1fx%.1f nodes=%zu as_tab=%d\n",
               (unsigned)d.target, (int)ws.valid(d.target), tb.x, tb.y, tb.width, tb.height,
               builder.nodes().size(), (int)d.as_tab);
    }
    ASSERT_NE(preview, nullptr) << "no drop preview was emitted while a card drag was in flight";

    // Every docked panel is content or panels layer, so the preview outranks all of them by LAYER
    // rather than by out-numbering their z — which is the whole point of the change.
    for (const layout_node& n : builder.nodes())
        if (&n != preview)
            EXPECT_LE(static_cast<int>(n.element.layer), static_cast<int>(ui_layer::panels))
                << "something other than the preview claimed the popup layer";

    // ...and below the menu, which sits at popups z=2. Preserves the order the old 253-vs-255 gave.
    EXPECT_LT(preview->element.z, 2) << "the preview must stay under the menu bar";
}
