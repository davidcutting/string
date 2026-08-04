#include <gtest/gtest.h>

#include <string/ui/layout.hpp>
#include <string/ui/interaction.hpp>
#include <string/ui/motion.hpp>

using namespace string;
using namespace string::ui;

namespace
{
// Two side-by-side id'd boxes in a row, plus an overlay panel covering the LEFT one. Exercises
// hover, focus nav (left/right) and the overlay-beats-main hit-test rule in one tree.
//
//   +----------+----------+
//   |  left    |  right   |     overlay sits on top of `left`
//   +----------+----------+
void author(layout_builder& b, bool with_overlay)
{
    element root{};
    root.sizing = size_grow();
    b.begin(root, format{ .direction = direction::HORIZONTAL });

    if (with_overlay)
    {
        // Added FIRST, deliberately: a later main-layer node then sits above it in tree order, so
        // "last hit wins" (what layout_builder::hit_test does) picks the WRONG node. That is the
        // real-world shape — a floating panel authored before the content it covers — and the only
        // fixture that actually discriminates between the two hit-tests.
        element over{};
        over.id = make_id("overlay");
        over.sizing = size_fixed(100, 50);
        over.floating = true;
        over.float_x = 0;
        over.float_y = 0;
        over.layer = ui_layer::panels;
        over.focusable = true;
        b.add_element(over);
    }

    element left{};
    left.id = make_id("left");
    left.sizing = size_fixed(100, 50);
    left.focusable = true;
    b.add_element(left);

    element right{};
    right.id = make_id("right");
    right.sizing = size_fixed(100, 50);
    right.focusable = true;
    b.add_element(right);

    b.end(dimension{ 200, 50 });
}

interaction_input at(float x, float y)
{
    interaction_input in;
    in.cursor_x = x;
    in.cursor_y = y;
    in.ui_mode = true;
    in.dt = 0.016f;
    in.screen = { 200, 50 };
    return in;
}
}  // namespace

// --- L1: the value-type contract ---------------------------------------------------------------

// The load-bearing constraint from brief 12 M0b: `interaction` is a plain value. If this ever fails
// to compile, the deferred "engine owns ui::host" step has silently become a rewrite.
TEST(UiInteractionTest, StateIsAPlainCopyableValue)
{
    static_assert(std::is_trivially_copyable_v<interaction>);
    static_assert(std::is_trivially_copyable_v<interaction_input>);
    static_assert(std::is_default_constructible_v<interaction>);

    interaction a{};
    a.hovered = 42;
    interaction b = a;   // copy, no aliasing back to any host
    EXPECT_EQ(b.hovered, 42u);
    EXPECT_TRUE(b.is_hot(42));
    EXPECT_FALSE(b.is_hot(0));   // id 0 is never "hot" — it means "nothing"
}

// --- L1: hover / focus ---------------------------------------------------------------------------

TEST(UiInteractionTest, ResolvesHoverFromCursor)
{
    layout_builder b;
    author(b, /*with_overlay=*/false);

    interaction s{};
    resolve_interaction(s, at(150, 25), b);
    EXPECT_EQ(s.hovered, make_id("right").hash);

    resolve_interaction(s, at(50, 25), b);
    EXPECT_EQ(s.hovered, make_id("left").hash);
}

// 12b M2: `hovered_box` is PUBLISHED state — widgets anchor tooltips and popups to it and compare
// it against raw cursor coordinates. A node inside a placed surface stores a SURFACE-LOCAL box, so
// publishing it unconverted hands every consumer a rect near the origin instead of one under the
// cursor. The bug is invisible to a layout dump (which never contains hovered_box) and to any
// headless capture (which never hovers), so it gets its own test.
TEST(UiInteractionTest, HoveredBoxIsPublishedInScreenSpace)
{
    layout_builder b;
    element root{};
    root.sizing = size_grow();
    b.begin(root, format{ .direction = direction::HORIZONTAL });
    {
        // A surface: root-space floating, well away from the origin.
        element over{};
        over.id = make_id("plate");
        over.sizing = size_fixed(60, 20);
        over.floating = true;
        over.float_x = 120;
        over.float_y = 25;
        over.layer = ui_layer::panels;
        b.add_element(over);
    }
    b.end(dimension{ 200, 50 });

    const layout_node* plate = b.find(make_id("plate").hash);
    ASSERT_NE(plate, nullptr);
    ASSERT_EQ(plate->box.x, 0) << "precondition: the node's own box is surface-local";

    interaction s{};
    resolve_interaction(s, at(150, 30), b);
    ASSERT_EQ(s.hovered, make_id("plate").hash) << "the cursor is not over the plate";
    EXPECT_EQ(s.hovered_box.x, 120) << "hovered_box was published in surface coordinates";
    EXPECT_EQ(s.hovered_box.y, 25);
    EXPECT_EQ(s.hovered_box.dimension.width, 60);
}

// The overlay layer draws after ALL main content, so it must also hit-test above it — otherwise a
// modal/floating panel is click-through. `layout_builder::hit_test` alone gets this wrong.
TEST(UiInteractionTest, OverlayBeatsMainLayerInHitTest)
{
    layout_builder b;
    author(b, /*with_overlay=*/true);

    // The fixture authors the overlay FIRST, so a hit test that only knew tree order would pick the
    // later main-layer node instead. (There used to be exactly such a hit test on layout_builder;
    // it was deleted rather than left as a second answer to the same question.)
    const layout_node* hit = hit_test_layered(b, 50, 25);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->element.id.hash, make_id("overlay").hash);
}

TEST(UiInteractionTest, ClickFocusesHoveredAndEmptySpaceRequestsGameMode)
{
    layout_builder b;
    author(b, false);

    interaction s{};
    interaction_input in = at(150, 25);
    resolve_interaction(s, in, b);      // hover `right`
    in.primary_pressed = true;
    resolve_interaction(s, in, b);
    EXPECT_EQ(s.focused, make_id("right").hash);
    EXPECT_FALSE(s.wants_game_mode);

    // A click on empty UI space falls through as a game-mode request, clearing focus.
    interaction_input empty = at(199, 49);
    empty.cursor_x = 300;               // outside every box
    empty.primary_pressed = true;
    resolve_interaction(s, empty, b);
    EXPECT_EQ(s.focused, 0u);
    EXPECT_TRUE(s.wants_game_mode);
}

TEST(UiInteractionTest, GameModeClearsFocus)
{
    layout_builder b;
    author(b, false);
    interaction s{};
    s.focused = make_id("right").hash;

    interaction_input in = at(150, 25);
    in.ui_mode = false;                 // mouse-look
    resolve_interaction(s, in, b);
    EXPECT_EQ(s.focused, 0u);
}

// --- L1: directional focus nav -------------------------------------------------------------------

TEST(UiInteractionTest, FocusNavMovesToNearestInDirection)
{
    layout_builder b;
    author(b, false);

    EXPECT_EQ(nearest_focusable(b, make_id("left").hash, 1, 0, dimension{ 200, 50 }),
              make_id("right").hash);
    EXPECT_EQ(nearest_focusable(b, make_id("right").hash, -1, 0, dimension{ 200, 50 }),
              make_id("left").hash);
    // Nothing lies above the row.
    EXPECT_EQ(nearest_focusable(b, make_id("left").hash, 0, -1, dimension{ 200, 50 }), 0u);
    // No direction requested -> no move.
    EXPECT_EQ(nearest_focusable(b, make_id("left").hash, 0, 0, dimension{ 200, 50 }), 0u);
}

// --- Focusable is NOT the same as id'd ----------------------------------------------------------
//
// The bug this locks down: nav used to take any node with an id, and an id is what a drag handle
// needs to be addressable at all. So pushing the stick in a panel-heavy screen landed focus on a
// resize grip — an element with nothing a stick can do to it.

namespace
{
// `left` and `right` as before, plus a `grip` between them that is addressable (hoverable,
// draggable) but NOT a focus target. It sits directly in nav's path, so a nav that ignores the
// distinction cannot avoid picking it.
void author_with_chrome(layout_builder& b)
{
    element root{};
    root.sizing = size_grow();
    b.begin(root, format{ .direction = direction::HORIZONTAL });

    element left{};
    left.id = make_id("left");
    left.sizing = size_fixed(60, 50);
    left.focusable = true;
    b.add_element(left);

    element grip{};
    grip.id = make_id("grip");           // id'd: it hovers and drags...
    grip.sizing = size_fixed(20, 50);    // ...but never declares focusable
    b.add_element(grip);

    element right{};
    right.id = make_id("right");
    right.sizing = size_fixed(60, 50);
    right.focusable = true;
    b.add_element(right);

    b.end(dimension{ 140, 50 });
}
}  // namespace

TEST(UiInteractionTest, NavSkipsAddressableButNonFocusableChrome)
{
    layout_builder b;
    author_with_chrome(b);

    // `grip` is nearer to `left` than `right` is, so a nav that only checked for an id would take
    // it. Focus must step over it to the next real control.
    EXPECT_EQ(nearest_focusable(b, make_id("left").hash, 1, 0, dimension{ 140, 50 }),
              make_id("right").hash);
    EXPECT_EQ(nearest_focusable(b, make_id("right").hash, -1, 0, dimension{ 140, 50 }),
              make_id("left").hash);
}

// A zero-area node cannot be seen, so focus landing on it is an invisible trap.
TEST(UiInteractionTest, NavSkipsZeroAreaNodes)
{
    layout_builder b;
    element root{};
    root.sizing = size_grow();
    b.begin(root, format{ .direction = direction::HORIZONTAL });

    element from{};
    from.id = make_id("from");
    from.sizing = size_fixed(40, 50);
    from.focusable = true;
    b.add_element(from);

    element collapsed{};
    collapsed.id = make_id("collapsed");
    collapsed.sizing = size_fixed(0, 0);   // focusable, but nothing is drawn
    collapsed.focusable = true;
    b.add_element(collapsed);

    element far{};
    far.id = make_id("far");
    far.sizing = size_fixed(40, 50);
    far.focusable = true;
    b.add_element(far);

    b.end(dimension{ 140, 50 });

    EXPECT_EQ(nearest_focusable(b, make_id("from").hash, 1, 0, dimension{ 140, 50 }),
              make_id("far").hash);
}

// Clicking chrome must not PARK focus there either. Otherwise the next stick push is measured from
// a resize grip, which is the same bug arriving by a different route.
TEST(UiInteractionTest, ClickingNonFocusableChromeClearsFocusRatherThanTakingIt)
{
    layout_builder b;
    author_with_chrome(b);

    interaction s{};
    s.focused = make_id("left").hash;

    interaction_input in = at(70, 25);   // over the grip
    in.primary_pressed = true;
    in.screen = { 140, 50 };
    resolve_interaction(s, in, b);

    EXPECT_EQ(s.hovered, make_id("grip").hash);   // still fully addressable...
    EXPECT_EQ(s.focused, 0u);                     // ...but not a place focus can rest
    // A click on real chrome is not a click on empty space, so it must NOT fall through to the
    // game-mode request the way a click on the background does.
    EXPECT_FALSE(s.wants_game_mode);
}

TEST(UiInteractionTest, ClickingAFocusableElementStillFocusesIt)
{
    layout_builder b;
    author_with_chrome(b);

    interaction s{};
    interaction_input in = at(110, 25);   // over `right`
    in.primary_pressed = true;
    in.screen = { 140, 50 };
    resolve_interaction(s, in, b);

    EXPECT_EQ(s.focused, make_id("right").hash);
}

TEST(UiInteractionTest, NavIntentRequestsUiMode)
{
    layout_builder b;
    author(b, false);
    interaction s{};
    s.focused = make_id("left").hash;

    interaction_input in = at(0, 0);
    in.nav_x = 1;
    resolve_interaction(s, in, b);
    EXPECT_TRUE(s.wants_ui_mode);
    EXPECT_EQ(s.focused, make_id("right").hash);
}

// --- L1: drag + pointer capture (the new machinery docking needs) --------------------------------

TEST(UiInteractionTest, PressTakesPointerCaptureAndTracksDelta)
{
    layout_builder b;
    author(b, false);

    interaction s{};
    resolve_interaction(s, at(50, 25), b);       // hover `left`
    ASSERT_EQ(s.hovered, make_id("left").hash);

    interaction_input press = at(50, 25);
    press.primary_pressed = true;
    press.primary_down = true;
    begin_interaction(s, press);
    EXPECT_TRUE(s.dragging());
    EXPECT_TRUE(s.is_active(make_id("left").hash));
    EXPECT_EQ(s.press_x, 50.0f);
    EXPECT_EQ(s.drag_x, 0.0f);

    interaction_input held = at(140, 40);
    held.primary_down = true;
    begin_interaction(s, held);
    EXPECT_FLOAT_EQ(s.drag_x, 90.0f);
    EXPECT_FLOAT_EQ(s.drag_y, 15.0f);
}

// The whole point of pointer capture: a drag that outruns its own box keeps the handle. Without
// this, resize/splitter drags drop the moment the cursor crosses into a sibling.
TEST(UiInteractionTest, DragOwnerKeepsHoverWhenCursorLeavesItsBox)
{
    layout_builder b;
    author(b, false);

    interaction s{};
    resolve_interaction(s, at(50, 25), b);
    interaction_input press = at(50, 25);
    press.primary_pressed = true;
    press.primary_down = true;
    begin_interaction(s, press);

    // Cursor is now over `right`, but `left` owns the pointer.
    interaction_input moved = at(150, 25);
    moved.primary_down = true;
    begin_interaction(s, moved);
    resolve_interaction(s, moved, b);
    EXPECT_EQ(s.hovered, make_id("left").hash);
    EXPECT_TRUE(s.is_active(make_id("left").hash));
}

TEST(UiInteractionTest, ReleaseEndsTheDrag)
{
    layout_builder b;
    author(b, false);

    interaction s{};
    resolve_interaction(s, at(50, 25), b);
    interaction_input press = at(50, 25);
    press.primary_pressed = true;
    press.primary_down = true;
    begin_interaction(s, press);
    ASSERT_TRUE(s.dragging());

    interaction_input release = at(60, 25);
    release.primary_released = true;
    release.primary_down = false;
    begin_interaction(s, release);
    EXPECT_FALSE(s.dragging());
    EXPECT_EQ(s.drag_x, 0.0f);
}

// Losing UI mode mid-drag (the player recaptured the mouse for camera look) must not leave a
// dangling drag owner that never releases.
TEST(UiInteractionTest, LeavingUiModeCancelsTheDrag)
{
    layout_builder b;
    author(b, false);

    interaction s{};
    resolve_interaction(s, at(50, 25), b);
    interaction_input press = at(50, 25);
    press.primary_pressed = true;
    press.primary_down = true;
    begin_interaction(s, press);
    ASSERT_TRUE(s.dragging());

    interaction_input captured = at(50, 25);
    captured.primary_down = true;
    captured.ui_mode = false;
    begin_interaction(s, captured);
    EXPECT_FALSE(s.dragging());
}

TEST(UiInteractionTest, GamepadActivatesFocusedElement)
{
    interaction s{};
    s.focused = make_id("right").hash;

    interaction_input in = at(0, 0);
    in.actions = action_bit(ui_action::activate);
    begin_interaction(s, in);
    EXPECT_TRUE(s.is_pressed(make_id("right").hash));
}

// --- L2: motion ----------------------------------------------------------------------------------

// First sight of a key snaps to the target: a newly-shown element must not slide in from 0.
TEST(UiMotionTest, FirstAppearanceSnapsToTarget)
{
    Motion m;
    m.begin_frame(0.016f);
    EXPECT_FLOAT_EQ(m.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f }), 255.0f);
}

TEST(UiMotionTest, GlidesTowardTargetOverFrames)
{
    Motion m;
    m.begin_frame(0.016f);
    m.animate(1, Motion::Prop::A, 0.0f, Transition{ 0.2f });   // establish at 0

    float v = 0.0f;
    for (int i = 0; i < 5; ++i)
    {
        m.begin_frame(0.016f);
        v = m.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f });
    }
    EXPECT_GT(v, 0.0f);      // moved
    EXPECT_LT(v, 255.0f);    // but not instantly

    for (int i = 0; i < 200; ++i)
    {
        m.begin_frame(0.016f);
        v = m.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f });
    }
    EXPECT_FLOAT_EQ(v, 255.0f);   // and settles exactly, no drift
}

// --- 12b M0b: Transition::delay ------------------------------------------------------------------

// The value HOLDS until the delay elapses, then eases. The clock starts when the TARGET IS WRITTEN,
// so the hold is armed by the change, not by the element appearing.
TEST(UiMotionTest, DelayHoldsThenEases)
{
    Motion m;
    m.begin_frame(0.016f);
    m.animate(1, Motion::Prop::A, 0.0f, Transition{ 0.2f, Curve::LINEAR, 0.1f });   // establish at 0

    // 0.096s of holding: 6 frames at 16ms, all still short of the 100ms delay.
    float v = -1.0f;
    for (int i = 0; i < 6; ++i)
    {
        m.begin_frame(0.016f);
        v = m.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f, Curve::LINEAR, 0.1f });
        EXPECT_FLOAT_EQ(v, 0.0f) << "moved during the hold, on frame " << i;
    }

    // The 7th frame crosses 0.1s, so easing starts — with only the 4ms remainder, not a full frame.
    m.begin_frame(0.016f);
    v = m.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f, Curve::LINEAR, 0.1f });
    EXPECT_GT(v, 0.0f) << "the hold expired but nothing moved";

    for (int i = 0; i < 200; ++i)
    {
        m.begin_frame(0.016f);
        v = m.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f, Curve::LINEAR, 0.1f });
    }
    EXPECT_FLOAT_EQ(v, 255.0f) << "a delayed transition must still settle exactly";
}

// Stagger, which is the whole reason the field exists: same target, same duration, delays offset by
// index. They must reach the target in that order and not together.
TEST(UiMotionTest, StaggeredEntriesCompleteOffsetByTheirDelays)
{
    Motion m;
    const auto tr = [](float delay) { return Transition{ 0.05f, Curve::LINEAR, delay }; };

    m.begin_frame(0.016f);
    for (int i = 0; i < 3; ++i)
        m.animate(static_cast<std::uint64_t>(i + 1), Motion::Prop::A, 0.0f, tr(0.03f * i));

    // Frame at which each entry first reaches the target.
    int done[3] = { -1, -1, -1 };
    for (int f = 0; f < 100; ++f)
    {
        m.begin_frame(0.016f);
        for (int i = 0; i < 3; ++i)
        {
            const float v =
                m.animate(static_cast<std::uint64_t>(i + 1), Motion::Prop::A, 255.0f, tr(0.03f * i));
            if (done[i] < 0 && v == 255.0f) done[i] = f;
        }
    }

    ASSERT_GE(done[0], 0) << "entry 0 never finished";
    ASSERT_GE(done[2], 0) << "entry 2 never finished";
    EXPECT_LT(done[0], done[1]) << "delays did not stagger: " << done[0] << "," << done[1];
    EXPECT_LT(done[1], done[2]) << "delays did not stagger: " << done[1] << "," << done[2];
}

// Writing a NEW target during the hold restarts it — "the clock starts when the target is written"
// has to mean this for a state that changes its mind mid-hold (hover on, then off).
TEST(UiMotionTest, WritingANewTargetDuringTheHoldRestartsIt)
{
    Motion m;
    const Transition tr{ 0.05f, Curve::LINEAR, 0.1f };
    m.begin_frame(0.016f);
    m.animate(1, Motion::Prop::A, 0.0f, tr);

    for (int i = 0; i < 4; ++i)   // 64ms into the 100ms hold
    {
        m.begin_frame(0.016f);
        m.animate(1, Motion::Prop::A, 255.0f, tr);
    }
    // Change the target: the hold restarts, so 64ms later it must STILL be holding.
    for (int i = 0; i < 4; ++i)
    {
        m.begin_frame(0.016f);
        EXPECT_FLOAT_EQ(m.animate(1, Motion::Prop::A, 128.0f, tr), 0.0f)
            << "the hold did not restart when the target changed";
    }
}

// delay = 0 must be bit-identical to before the field existed. Not "close" — the same float, frame
// by frame — or every existing animation in the UI has silently changed.
TEST(UiMotionTest, ZeroDelayIsBitIdenticalToNoDelay)
{
    for (const Curve c : { Curve::LINEAR, Curve::EASE_OUT, Curve::EASE_IN_OUT, Curve::EASE_OUT_BACK,
                           Curve::SPRING })
    {
        Motion plain;
        Motion zero;
        plain.begin_frame(0.016f);
        zero.begin_frame(0.016f);
        plain.animate(1, Motion::Prop::A, 0.0f, Transition{ 0.2f, c });
        zero.animate(1, Motion::Prop::A, 0.0f, Transition{ 0.2f, c, 0.0f });

        for (int i = 0; i < 60; ++i)
        {
            plain.begin_frame(0.016f);
            zero.begin_frame(0.016f);
            const float a = plain.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f, c });
            const float b = zero.animate(1, Motion::Prop::A, 255.0f, Transition{ 0.2f, c, 0.0f });
            ASSERT_EQ(a, b) << "curve " << static_cast<int>(c) << " diverged on frame " << i;
        }
    }
}

// A delayed entry must age out like any other: the hold must not keep it alive.
TEST(UiMotionTest, ADelayedEntryStillAgesOut)
{
    Motion m;
    m.begin_frame(0.016f);
    m.animate(1, Motion::Prop::A, 0.0f, Transition{ 0.2f, Curve::EASE_OUT, 1.0f });
    m.end_frame();
    ASSERT_EQ(m.live_count(), 1u);

    m.begin_frame(0.016f);   // not touched — still mid-hold, but gone all the same
    m.end_frame();
    EXPECT_EQ(m.live_count(), 0u);
}

// Entries untouched for a frame are aged out — a rebuilt/removed element must not leak state.
TEST(UiMotionTest, UntouchedEntriesAgeOut)
{
    Motion m;
    m.begin_frame(0.016f);
    m.animate(1, Motion::Prop::A, 10.0f, {});
    m.animate(2, Motion::Prop::A, 10.0f, {});
    m.end_frame();
    EXPECT_EQ(m.live_count(), 2u);

    m.begin_frame(0.016f);
    m.animate(1, Motion::Prop::A, 10.0f, {});   // only id 1 this frame
    m.end_frame();
    EXPECT_EQ(m.live_count(), 1u);
}

TEST(UiMotionTest, SpringSettlesExactlyOnTarget)
{
    Motion m;
    m.begin_frame(0.016f);
    m.animate(7, Motion::Prop::Scale, 0.0f, Transition{ 0.2f, Curve::SPRING });

    float v = 0.0f;
    for (int i = 0; i < 400; ++i)
    {
        m.begin_frame(0.016f);
        v = m.animate(7, Motion::Prop::Scale, 1.0f, Transition{ 0.2f, Curve::SPRING });
    }
    EXPECT_FLOAT_EQ(v, 1.0f);
}

TEST(UiMotionTest, EaseCurvesAreBounded)
{
    for (const Curve c : { Curve::LINEAR, Curve::EASE_IN, Curve::EASE_OUT, Curve::EASE_IN_OUT })
    {
        EXPECT_FLOAT_EQ(ease(c, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(ease(c, 1.0f), 1.0f);
    }
    // EASE_OUT_BACK deliberately overshoots in the middle but still lands on the endpoints.
    EXPECT_FLOAT_EQ(ease(Curve::EASE_OUT_BACK, 1.0f), 1.0f);
}

// --- Routed actions + focus scopes ---------------------------------------------------------------
//
// Actions replace the hardcoded `activate`/`submit` flags. The point is not remapping alone (that
// happens host-side); it is that exactly ONE element receives each action, decided centrally. With a
// field inside a dialog inside a panel, "whoever checks first" is not a rule — it is a bug that
// depends on authoring order.

namespace
{
// A dialog (modal scope, handles cancel) containing a field (handles submit), plus a button OUTSIDE
// the dialog that also handles both. The outside button is the trap: if bubbling or nav escaped the
// modal, it is what they would wrongly reach.
void author_dialog(layout_builder& b)
{
    element root{};
    root.sizing = size_grow();
    b.begin(root, format{ .direction = direction::VERTICAL });

    element outside{};
    outside.id = make_id("outside");
    outside.sizing = size_fixed(100, 40);
    outside.focusable = true;
    outside.actions = action_bit(ui_action::cancel) | action_bit(ui_action::submit);
    b.add_element(outside);

    element dialog{};
    dialog.id = make_id("dialog");
    dialog.sizing = size_fixed(200, 100);
    dialog.modal = true;                              // implies scope
    dialog.actions = action_bit(ui_action::cancel);
    b.begin(dialog, format{ .direction = direction::VERTICAL });
    {
        element field{};
        field.id = make_id("field");
        field.sizing = size_fixed(180, 30);
        field.focusable = true;
        field.actions = action_bit(ui_action::submit);
        b.add_element(field);

        element ok{};
        ok.id = make_id("ok");
        ok.sizing = size_fixed(60, 30);
        ok.focusable = true;
        b.add_element(ok);
    }
    b.end();

    b.end(dimension{ 300, 200 });
}

interaction_input with_actions(std::uint32_t actions)
{
    interaction_input in;
    in.ui_mode = true;
    in.cursor_x = -1;   // off every box, so hover does not interfere
    in.cursor_y = -1;
    in.screen = { 300, 200 };
    in.actions = actions;
    return in;
}
}  // namespace

TEST(UiInteractionTest, ActionGoesToTheFocusedElementThatDeclaresIt)
{
    layout_builder b;
    author_dialog(b);
    interaction s{};
    s.focused = make_id("field").hash;

    resolve_interaction(s, with_actions(action_bit(ui_action::submit)), b);
    EXPECT_TRUE(s.took(make_id("field").hash, ui_action::submit));
    EXPECT_FALSE(s.took(make_id("outside").hash, ui_action::submit))
        << "the outside handler must not also fire — exactly one target";
}

// The composition that makes this worth having: the field knows nothing about cancel, the dialog
// knows nothing about the field, and Escape still reaches the dialog.
TEST(UiInteractionTest, UnhandledActionBubblesToTheAncestorThatDeclaresIt)
{
    layout_builder b;
    author_dialog(b);
    interaction s{};
    s.focused = make_id("field").hash;

    resolve_interaction(s, with_actions(action_bit(ui_action::cancel)), b);
    EXPECT_TRUE(s.took(make_id("dialog").hash, ui_action::cancel));
    EXPECT_FALSE(s.took(make_id("field").hash, ui_action::cancel));
}

// What "modal" means: a surface swallows what it does not handle, rather than letting the thing
// BEHIND it act on input the user aimed at the dialog.
TEST(UiInteractionTest, ModalSwallowsAnActionItDoesNotHandle)
{
    layout_builder b;
    author_dialog(b);
    interaction s{};
    s.focused = make_id("ok").hash;   // `ok` handles nothing

    // `outside` declares submit, but the modal is between them.
    resolve_interaction(s, with_actions(action_bit(ui_action::submit)), b);
    EXPECT_FALSE(s.took(make_id("outside").hash, ui_action::submit));
    EXPECT_EQ(s.action_target[static_cast<std::size_t>(ui_action::submit)], 0u);
}

// Escape closing the frontmost surface must not require having tabbed into it first.
TEST(UiInteractionTest, WithNoFocusAnActionGoesToTheTopmostHandler)
{
    layout_builder b;
    element root{};
    root.sizing = size_grow();
    b.begin(root, format{ .direction = direction::VERTICAL });

    element low{};
    low.id = make_id("low");
    low.sizing = size_fixed(100, 40);
    low.actions = action_bit(ui_action::cancel);
    b.add_element(low);

    element high{};
    high.id = make_id("high");
    high.sizing = size_fixed(100, 40);
    high.actions = action_bit(ui_action::cancel);
    high.layer = ui_layer::panels;            // drawn above, so it is the frontmost surface
    high.floating = true;
    b.add_element(high);
    b.end(dimension{ 300, 200 });

    interaction s{};
    ASSERT_EQ(s.focused, 0u);
    resolve_interaction(s, with_actions(action_bit(ui_action::cancel)), b);
    EXPECT_TRUE(s.took(make_id("high").hash, ui_action::cancel));
}

// A target is an EVENT. A stale one would re-fire a dialog's cancel every frame after the key came
// up, which is the immediate-mode trap this whole table has to avoid.
TEST(UiInteractionTest, ActionTargetsClearWhenNoActionArrives)
{
    layout_builder b;
    author_dialog(b);
    interaction s{};
    s.focused = make_id("field").hash;

    resolve_interaction(s, with_actions(action_bit(ui_action::submit)), b);
    ASSERT_TRUE(s.took(make_id("field").hash, ui_action::submit));
    resolve_interaction(s, with_actions(0), b);
    EXPECT_FALSE(s.took(make_id("field").hash, ui_action::submit));
}

// Nav confinement: the stick must not walk out of an open dialog into what is behind it, which
// would look exactly like the dialog failing to be modal.
TEST(UiInteractionTest, NavIsConfinedToTheFocusedElementsScope)
{
    layout_builder b;
    author_dialog(b);

    // `outside` sits ABOVE the dialog in the layout, and is focusable — the only thing keeping nav
    // off it is the scope.
    EXPECT_EQ(nearest_focusable(b, make_id("field").hash, 0, -1, dimension{ 300, 200 }), 0u);
    // ...while movement WITHIN the dialog still works.
    EXPECT_EQ(nearest_focusable(b, make_id("field").hash, 0, 1, dimension{ 300, 200 }),
              make_id("ok").hash);
}

// Outside any scope, the whole tree is in play — confinement is opt-in, like everything else here.
TEST(UiInteractionTest, NavIsUnconfinedWhenFocusIsInNoScope)
{
    layout_builder b;
    author_with_chrome(b);
    EXPECT_EQ(nearest_focusable(b, make_id("left").hash, 1, 0, dimension{ 140, 50 }),
              make_id("right").hash);
}
