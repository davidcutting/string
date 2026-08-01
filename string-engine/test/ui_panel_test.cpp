#include <gtest/gtest.h>

#include <string/ui/panel.hpp>
#include <string/ui/ui.hpp>

using namespace string;
using namespace string::ui;

namespace
{
constexpr std::uint64_t MOVE = 0x1111;
constexpr std::uint64_t GRIP = 0x2222;
constexpr dimension SCREEN{ 1920, 1080 };

panel_handles handles() { return panel_handles{ MOVE, GRIP }; }

// A drag in flight: `active` owns the pointer and drag_x/y is the absolute delta from the press.
interaction dragging(std::uint64_t owner, float dx, float dy)
{
    interaction ia{};
    ia.active = owner;
    ia.hovered = owner;
    ia.drag_x = dx;
    ia.drag_y = dy;
    ia.screen = SCREEN;
    return ia;
}

interaction idle()
{
    interaction ia{};
    ia.screen = SCREEN;
    return ia;
}
}  // namespace

// --- Store ---------------------------------------------------------------------------------------

TEST(UiPanelTest, StoreAppliesInitialRectOnlyOnFirstUse)
{
    PanelStore store;
    panel_state& a = store.panel(7, { 10, 20, 300, 200 });
    EXPECT_FLOAT_EQ(a.rect.x, 10.0f);
    a.rect.x = 500.0f;

    // A second frame passing a different initial must NOT stomp the stored rect — otherwise an
    // author computing a default position each frame would fight every drag.
    panel_state& b = store.panel(7, { 10, 20, 300, 200 });
    EXPECT_FLOAT_EQ(b.rect.x, 500.0f);
    EXPECT_EQ(store.size(), 1u);
}

TEST(UiPanelTest, StoreFindReturnsNullForUnknownPanel)
{
    PanelStore store;
    EXPECT_EQ(store.find(99), nullptr);
    store.panel(99, {});
    EXPECT_NE(store.find(99), nullptr);
}

// --- Move ----------------------------------------------------------------------------------------

TEST(UiPanelTest, TitleDragMovesPanelByDelta)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    update_panel(st, dragging(MOVE, 40, -30), handles(), {}, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.x, 140.0f);
    EXPECT_FLOAT_EQ(st.rect.y, 70.0f);
    // Size is untouched by a move.
    EXPECT_FLOAT_EQ(st.rect.width, 300.0f);
    EXPECT_FLOAT_EQ(st.rect.height, 200.0f);
}

// THE load-bearing property. drag_x/y is absolute-from-press, so the rect is recomputed from the
// origin each frame rather than accumulated. A naive `rect += delta` would move the panel by 40,
// then by 90, then by 150 across these three frames instead of tracking the cursor.
TEST(UiPanelTest, DragIsAbsoluteFromOriginNotAccumulated)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    const panel_handles h = handles();
    update_panel(st, dragging(MOVE, 40, 0), h, {}, SCREEN);
    update_panel(st, dragging(MOVE, 90, 0), h, {}, SCREEN);
    update_panel(st, dragging(MOVE, 150, 0), h, {}, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.x, 250.0f);
}

// The reason clamping must be idempotent: drag hard past the edge, then come back. An
// accumulate-and-clamp implementation eats the clamped motion and the panel lags the cursor.
TEST(UiPanelTest, DraggingPastTheEdgeAndBackReturnsToTheCursor)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    const panel_handles h = handles();
    update_panel(st, dragging(MOVE, 5000, 0), h, {}, SCREEN);   // way off the right edge
    EXPECT_LT(st.rect.x, static_cast<float>(SCREEN.width));     // clamped
    update_panel(st, dragging(MOVE, 50, 0), h, {}, SCREEN);     // back to a modest delta
    EXPECT_FLOAT_EQ(st.rect.x, 150.0f);                         // exactly origin + delta again
}

TEST(UiPanelTest, MoveKeepsAStripOnScreen)
{
    panel_limits lim{};
    panel_state st{ { 100, 100, 300, 200 }, true };
    update_panel(st, dragging(MOVE, -5000, 5000), handles(), lim, SCREEN);
    // Left: at least `keep_visible` of the panel remains past x=0.
    EXPECT_GE(st.rect.right(), lim.keep_visible);
    // Bottom: the title bar must never leave the viewport, or the panel is unrecoverable.
    EXPECT_LE(st.rect.y, static_cast<float>(SCREEN.height));
    EXPECT_GE(st.rect.y, 0.0f);
}

TEST(UiPanelTest, TopEdgeIsClampedSoTheTitleBarStaysReachable)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    update_panel(st, dragging(MOVE, 0, -900), handles(), {}, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.y, 0.0f);
}

// --- Resize --------------------------------------------------------------------------------------

TEST(UiPanelTest, GripDragResizesFromBottomRightAndAnchorsTopLeft)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    update_panel(st, dragging(GRIP, 60, 40), handles(), {}, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.width, 360.0f);
    EXPECT_FLOAT_EQ(st.rect.height, 240.0f);
    EXPECT_FLOAT_EQ(st.rect.x, 100.0f);   // the anchor corner does not move
    EXPECT_FLOAT_EQ(st.rect.y, 100.0f);
}

TEST(UiPanelTest, ResizeHonoursMinimumSize)
{
    panel_limits lim{ 160.0f, 96.0f, 48.0f };
    panel_state st{ { 100, 100, 300, 200 }, true };
    update_panel(st, dragging(GRIP, -5000, -5000), handles(), lim, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.width, lim.min_width);
    EXPECT_FLOAT_EQ(st.rect.height, lim.min_height);
}

TEST(UiPanelTest, ResizeIsClampedToTheViewportButNeverBelowMinimum)
{
    panel_limits lim{ 160.0f, 96.0f, 48.0f };
    panel_state st{ { 100, 100, 300, 200 }, true };
    update_panel(st, dragging(GRIP, 9000, 9000), handles(), lim, SCREEN);
    EXPECT_LE(st.rect.right(), static_cast<float>(SCREEN.width) + 0.5f);
    EXPECT_LE(st.rect.bottom(), static_cast<float>(SCREEN.height) + 0.5f);
    EXPECT_GE(st.rect.width, lim.min_width);
}

// A panel parked near the right edge on a wide screen must not be squashed when the viewport
// shrinks below its minimum — min size wins, so it stays usable (and draggable back).
TEST(UiPanelTest, ResizeOnATinyViewportKeepsTheMinimum)
{
    panel_limits lim{ 160.0f, 96.0f, 48.0f };
    panel_state st{ { 0, 0, 300, 200 }, true };
    update_panel(st, dragging(GRIP, 10, 10), handles(), lim, dimension{ 100, 60 });
    EXPECT_FLOAT_EQ(st.rect.width, lim.min_width);
    EXPECT_FLOAT_EQ(st.rect.height, lim.min_height);
}

// --- Drag ownership ------------------------------------------------------------------------------

TEST(UiPanelTest, ReleasingClearsTheDragOwner)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    const panel_handles h = handles();
    update_panel(st, dragging(MOVE, 40, 0), h, {}, SCREEN);
    EXPECT_EQ(st.drag_owner, MOVE);
    update_panel(st, idle(), h, {}, SCREEN);
    EXPECT_EQ(st.drag_owner, 0u);
    EXPECT_FLOAT_EQ(st.rect.x, 140.0f);   // the moved position sticks
}

// Switching handles must re-snapshot the origin, or the second drag would be measured against the
// first drag's starting rect and jump.
TEST(UiPanelTest, SwitchingHandleResnapshotsTheOrigin)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    const panel_handles h = handles();
    update_panel(st, dragging(MOVE, 50, 0), h, {}, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.x, 150.0f);
    update_panel(st, dragging(GRIP, 20, 0), h, {}, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.width, 320.0f);   // 300 + 20, not 300 + 50 + 20
    EXPECT_FLOAT_EQ(st.rect.x, 150.0f);
}

// A drag on some OTHER element (a button elsewhere) must not move the panel.
TEST(UiPanelTest, ForeignDragIsIgnored)
{
    panel_state st{ { 100, 100, 300, 200 }, true };
    update_panel(st, dragging(0xDEAD, 400, 400), handles(), {}, SCREEN);
    EXPECT_FLOAT_EQ(st.rect.x, 100.0f);
    EXPECT_FLOAT_EQ(st.rect.y, 100.0f);
}

// A viewport shrink between frames must pull a stranded panel back into view even with no drag.
TEST(UiPanelTest, IdleFrameReclampsAfterViewportShrink)
{
    panel_state st{ { 1800, 1000, 300, 200 }, true };
    update_panel(st, idle(), handles(), {}, dimension{ 800, 600 });
    EXPECT_LE(st.rect.x, 800.0f);
    EXPECT_LE(st.rect.y, 600.0f);
}

// --- Through the facade --------------------------------------------------------------------------

namespace
{
struct Rig
{
    layout_builder builder;
    interaction state{};
    Motion motion;
    Theme theme{};
    PanelStore panels;
    Ui ui{ builder, state, motion, theme, panels };
};
}  // namespace

TEST(UiPanelTest, WindowEmitsChromeWithStableIdsAndPlacesItsBody)
{
    Rig rig;
    rig.state.screen = dimension{ 800, 600 };
    rig.ui.begin_frame();
    rig.builder.begin(format{ .direction = direction::VERTICAL });
    rig.ui.panel("stats").title("Stats").initial({ 50, 60, 320, 240 }).content([](Ui& u) {
        u.text("body");
    });
    rig.builder.end(dimension{ 800, 600 });

    const layout_node* outer = rig.builder.find(make_id("stats").hash);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->box.x, 50);
    EXPECT_EQ(outer->box.y, 60);
    EXPECT_EQ(outer->box.dimension.width, 320);
    EXPECT_EQ(outer->box.dimension.height, 240);

    // The two handles exist and are addressable by derived name.
    const layout_node* bar = rig.builder.find(make_id("stats.title").hash);
    const layout_node* grip = rig.builder.find(make_id("stats.grip").hash);
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(grip, nullptr);
    EXPECT_EQ(bar->box.dimension.width, 320);              // title bar spans the panel
    EXPECT_GT(grip->box.x, outer->box.x + 200);            // grip sits at the bottom-right
    EXPECT_GT(grip->box.y, outer->box.y + 150);
}

// The window's rect must come from the STORE, not the initial, once it has been dragged — this is
// the whole point of the state living outside the rebuilt-every-frame layout tree.
TEST(UiPanelTest, WindowRectSurvivesTheImmediateModeRebuild)
{
    Rig rig;
    rig.state.screen = dimension{ 800, 600 };

    auto author = [&] {
        rig.ui.begin_frame();
        rig.builder.clear();
        rig.builder.begin(format{ .direction = direction::VERTICAL });
        rig.ui.panel("stats").title("Stats").initial({ 50, 60, 320, 240 }).content([](Ui&) {});
        rig.builder.end(dimension{ 800, 600 });
    };

    author();
    // Now drag the title bar 30px right across a fresh frame.
    rig.state = dragging(make_id("stats.title").hash, 30, 10);
    rig.state.screen = dimension{ 800, 600 };
    author();

    const layout_node* outer = rig.builder.find(make_id("stats").hash);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->box.x, 80);
    EXPECT_EQ(outer->box.y, 70);
}

// A window's caption must be id-less, for the same reason a button's is: an id'd caption becomes
// the hover target and the bar stops being draggable wherever the text sits.
TEST(UiPanelTest, TitleBarCaptionDoesNotStealTheDragTarget)
{
    Rig rig;
    rig.state.screen = dimension{ 800, 600 };
    rig.ui.begin_frame();
    rig.builder.begin(format{ .direction = direction::VERTICAL });
    rig.ui.panel("stats").title("Stats").initial({ 50, 60, 320, 240 }).content([](Ui&) {});
    rig.builder.end(dimension{ 800, 600 });

    const layout_node* bar = rig.builder.find(make_id("stats.title").hash);
    ASSERT_NE(bar, nullptr);
    // Hit the middle of the title bar, where the caption is.
    const layout_node* hit = hit_test_layered(
        rig.builder, static_cast<std::uint16_t>(bar->box.x + bar->box.dimension.width / 2),
        static_cast<std::uint16_t>(bar->box.y + bar->box.dimension.height / 2));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->element.id.hash, make_id("stats.title").hash);
}

// --- Z-order -------------------------------------------------------------------------------------

TEST(UiPanelTest, StoreOrdersPanelsBackToFrontWithNewestOnTop)
{
    PanelStore store;
    store.panel(1, {});
    store.panel(2, {});
    store.panel(3, {});
    ASSERT_EQ(store.order().size(), 3u);
    EXPECT_EQ(store.order()[0], 1u);   // oldest at the back
    EXPECT_EQ(store.order()[2], 3u);   // newest on top
    EXPECT_EQ(store.depth_of(1), 0u);
    EXPECT_EQ(store.depth_of(3), 2u);
}

TEST(UiPanelTest, BringToFrontRaisesWithoutDisturbingTheRest)
{
    PanelStore store;
    for (std::uint64_t id : { 1, 2, 3, 4 }) store.panel(id, {});
    store.bring_to_front(2);
    // 2 goes to the top; 1, 3, 4 keep their relative order.
    EXPECT_EQ(store.order()[0], 1u);
    EXPECT_EQ(store.order()[1], 3u);
    EXPECT_EQ(store.order()[2], 4u);
    EXPECT_EQ(store.order()[3], 2u);
}

TEST(UiPanelTest, BringToFrontIsANoOpForFrontmostAndUnknown)
{
    PanelStore store;
    store.panel(1, {});
    store.panel(2, {});
    store.bring_to_front(2);            // already frontmost
    EXPECT_EQ(store.order()[1], 2u);
    store.bring_to_front(0xDEAD);       // unknown
    EXPECT_EQ(store.order().size(), 2u);
    EXPECT_EQ(store.order()[0], 1u);
}

// Draw order and hit order must agree, so the hit test reads the same key the renderer batches on:
// a HIGHER z wins even when the lower-z node is authored later (later = on top within a layer).
TEST(UiPanelTest, HigherZWinsTheHitTestOverLaterTreeOrder)
{
    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL });
    {
        element raised{};
        raised.id = make_id("raised");
        raised.sizing = size_fixed(100, 100);
        raised.floating = true;
        raised.overlay = true;
        raised.z = 5;
        b.add_element(raised);

        // Authored LATER (so it would win on tree order) but at a LOWER z.
        element buried{};
        buried.id = make_id("buried");
        buried.sizing = size_fixed(100, 100);
        buried.floating = true;
        buried.overlay = true;
        buried.z = 2;
        b.add_element(buried);
    }
    b.end(dimension{ 400, 400 });

    const layout_node* hit = hit_test_layered(b, 50, 50);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->element.id.hash, make_id("raised").hash);
}

// An overlay node must still beat a main-layer node regardless of z — overlay is the coarse layer,
// z orders within it. (Guards the bit-packing of the key.)
TEST(UiPanelTest, OverlayStillBeatsMainLayerWhateverTheZ)
{
    layout_builder b;
    b.begin(format{ .direction = direction::VERTICAL });
    {
        element over{};
        over.id = make_id("over");
        over.sizing = size_fixed(100, 100);
        over.floating = true;
        over.overlay = true;
        over.z = 1;
        b.add_element(over);

        element main{};
        main.id = make_id("main");
        main.sizing = size_fixed(100, 100);
        main.floating = true;
        main.z = 200;   // huge z, but NOT overlay
        b.add_element(main);
    }
    b.end(dimension{ 400, 400 });

    const layout_node* hit = hit_test_layered(b, 50, 50);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->element.id.hash, make_id("over").hash);
}

// --- Click to raise ------------------------------------------------------------------------------

namespace
{
// Authors two overlapping windows and returns the resulting store order.
void author_two(Rig& rig, std::uint64_t pressed)
{
    rig.state = interaction{};
    rig.state.screen = dimension{ 800, 600 };
    rig.state.pressed = pressed;
    rig.ui.begin_frame();
    rig.builder.clear();
    rig.builder.begin(format{ .direction = direction::VERTICAL });
    rig.ui.panel("a").title("A").initial({ 0, 0, 300, 300 }).content([](Ui& u) {
        u.button("a_btn").content([](Ui& u2) { u2.text("go"); });
    });
    rig.ui.panel("b").title("B").initial({ 100, 100, 300, 300 }).content([](Ui&) {});
    rig.builder.end(dimension{ 800, 600 });
}
}  // namespace

TEST(UiPanelTest, PressingAPanelRaisesItAboveTheOthers)
{
    Rig rig;
    author_two(rig, 0);
    // "b" was authored second, so it starts on top.
    EXPECT_EQ(rig.panels.depth_of(make_id("b").hash), 1u);

    author_two(rig, make_id("a.title").hash);   // press a's title bar
    EXPECT_EQ(rig.panels.depth_of(make_id("a").hash), 1u);
    EXPECT_EQ(rig.panels.depth_of(make_id("b").hash), 0u);
}

// The nested case: clicking a BUTTON inside a buried panel must raise the panel, not just a click on
// its own chrome. Ancestry is resolved at authoring time (see Ui::authoring_window_).
TEST(UiPanelTest, PressingAWidgetInsideAPanelRaisesThatPanel)
{
    Rig rig;
    author_two(rig, 0);
    ASSERT_EQ(rig.panels.depth_of(make_id("a").hash), 0u);   // buried

    author_two(rig, make_id("a_btn").hash);                  // press a nested button
    EXPECT_EQ(rig.panels.depth_of(make_id("a").hash), 1u);   // raised
}

// A press on something outside every panel must not reshuffle the stack.
TEST(UiPanelTest, PressingOutsideAnyPanelLeavesTheOrderAlone)
{
    Rig rig;
    author_two(rig, 0);
    const auto before_a = rig.panels.depth_of(make_id("a").hash);
    const auto before_b = rig.panels.depth_of(make_id("b").hash);
    author_two(rig, make_id("something_else").hash);
    EXPECT_EQ(rig.panels.depth_of(make_id("a").hash), before_a);
    EXPECT_EQ(rig.panels.depth_of(make_id("b").hash), before_b);
}

// The raised panel must actually carry a higher z into the tree — that is what the renderer batches
// on, so an order change that did not reach the elements would be invisible.
TEST(UiPanelTest, RaisedPanelCarriesAHigherZIntoTheTree)
{
    Rig rig;
    author_two(rig, 0);
    author_two(rig, make_id("a.title").hash);   // raise a
    author_two(rig, 0);                         // re-author so the new order reaches the tree

    const layout_node* a = rig.builder.find(make_id("a").hash);
    const layout_node* b = rig.builder.find(make_id("b").hash);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_GT(a->element.z, b->element.z);
}
