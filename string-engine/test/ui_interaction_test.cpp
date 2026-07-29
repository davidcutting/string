#include <gtest/gtest.h>

#include <string/core/layout.hpp>
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
        over.overlay = true;
        b.add_element(over);
    }

    element left{};
    left.id = make_id("left");
    left.sizing = size_fixed(100, 50);
    b.add_element(left);

    element right{};
    right.id = make_id("right");
    right.sizing = size_fixed(100, 50);
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

// The overlay layer draws after ALL main content, so it must also hit-test above it — otherwise a
// modal/floating panel is click-through. `layout_builder::hit_test` alone gets this wrong.
TEST(UiInteractionTest, OverlayBeatsMainLayerInHitTest)
{
    layout_builder b;
    author(b, /*with_overlay=*/true);

    const layout_node* hit = hit_test_layered(b, 50, 25);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->element.id.hash, make_id("overlay").hash);

    // ...and the plain hit-test does NOT — this is exactly the gap M0b closes.
    const layout_node* plain = b.hit_test(50, 25);
    ASSERT_NE(plain, nullptr);
    EXPECT_NE(plain->element.id.hash, make_id("overlay").hash);
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
    in.activate = true;
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
