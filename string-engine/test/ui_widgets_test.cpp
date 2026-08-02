#include <gtest/gtest.h>

#include <cmath>

#include <string/core/layout_dump.hpp>
#include <string/ui/widgets.hpp>

using namespace string;
using namespace string::ui;

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

    // One frame around an author callback, with a screen-filling root so grow/fit resolve.
    void frame(const std::function<void(Ui&)>& author, dimension screen = { 800, 600 })
    {
        state.screen = screen;
        ui.begin_frame();
        builder.clear();
        builder.begin(format{ .direction = direction::VERTICAL });
        author(ui);
        builder.end(screen);
        // The host's post-layout seam: sizes exist only now, and a widget that measures its own
        // content (a scroll area) reads what this caches on the NEXT frame.
        ui.observe();
        ui.end_frame();
        state.scroll_y = 0.0f;   // an EVENT: begin_interaction refills it from each frame's input
    }

    // Deliver wheel notches at a cursor position, in the order the host does it: the TARGET is
    // resolved post-layout against the tree that exists (resolve_interaction), and the NOTCHES arrive
    // at the top of the next frame (begin_interaction). Splitting them here is what keeps the test
    // honest about the one-frame seam.
    void wheel(float notches, int x, int y)
    {
        interaction_input in{};
        in.ui_mode = true;
        in.cursor_x = static_cast<float>(x);
        in.cursor_y = static_cast<float>(y);
        in.screen = state.screen;
        resolve_interaction(state, in, builder);
        state.scroll_y = notches;
    }
};
}  // namespace

// --- Checkbox --------------------------------------------------------------------------------------

TEST(UiWidgetsTest, CheckboxRoundTripsThroughItsBinding)
{
    Rig r;
    bool flag = false;
    struct Holder { bool v = false; } h;

    r.frame([&](Ui& u) { checkbox(u, "wire", bind(h, &Holder::v)).label("Wireframe"); });
    EXPECT_FALSE(h.v);

    // Press it: the binding flips, and the callback sees the NEW value.
    bool seen = false;
    r.state.pressed = make_id("wire").hash;
    r.frame([&](Ui& u) {
        checkbox(u, "wire", bind(h, &Holder::v)).label("Wireframe").on_change([&](bool v) {
            seen = v;
            flag = true;
        });
    });
    EXPECT_TRUE(h.v);
    EXPECT_TRUE(flag);
    EXPECT_TRUE(seen) << "the callback must receive the value it changed TO";
}

// Immediate mode's point: the frame you click on already shows the new state. If the box waited for
// the next frame the toggle would feel a frame late.
TEST(UiWidgetsTest, CheckboxShowsTheNewStateOnTheFrameItIsClicked)
{
    Rig r;
    struct Holder { bool v = false; } h;
    r.state.pressed = make_id("wire").hash;
    r.frame([&](Ui& u) { checkbox(u, "wire", bind(h, &Holder::v)); });

    // The box element is the checkbox row's first child; find the row and inspect its child fill.
    const layout_node* row = r.builder.find(make_id("wire").hash);
    ASSERT_NE(row, nullptr);
    ASSERT_NE(row->first_child, layout_node::none);
    const layout_node& box = r.builder.nodes()[row->first_child];
    EXPECT_EQ(box.element.color.r, r.theme.accent.r) << "must already be drawn as ON";
}

TEST(UiWidgetsTest, CheckboxCaptionIsIdLessSoTheRowStaysTheTarget)
{
    Rig r;
    struct Holder { bool v = false; } h;
    r.frame([&](Ui& u) { checkbox(u, "wire", bind(h, &Holder::v)).label("Wireframe"); });

    const layout_node* row = r.builder.find(make_id("wire").hash);
    ASSERT_NE(row, nullptr);
    const layout_node* hit = hit_test_layered(
        r.builder, static_cast<std::uint16_t>(row->box.x + row->box.dimension.width - 2),
        static_cast<std::uint16_t>(row->box.y + row->box.dimension.height / 2));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->element.id.hash, make_id("wire").hash);
}

// --- DragValue -------------------------------------------------------------------------------------

TEST(UiWidgetsTest, DragValueScrubsFromTheValueAtPress)
{
    Rig r;
    struct Holder { float v = 5.0f; } h;
    const std::uint64_t idh = make_id("speed").hash;

    // Press frame: capture the origin, no movement yet.
    r.state.active = idh;
    r.state.drag_x = 0.0f;
    r.frame([&](Ui& u) { drag_value(u, "speed", bind(h, &Holder::v)).range(0, 10).step(0.1f); });
    EXPECT_FLOAT_EQ(h.v, 5.0f);

    r.state.drag_x = 20.0f;
    r.frame([&](Ui& u) { drag_value(u, "speed", bind(h, &Holder::v)).range(0, 10).step(0.1f); });
    EXPECT_NEAR(h.v, 7.0f, 0.001f);

    // ABSOLUTE from the origin: a larger delta is not applied on top of the previous result.
    r.state.drag_x = 30.0f;
    r.frame([&](Ui& u) { drag_value(u, "speed", bind(h, &Holder::v)).range(0, 10).step(0.1f); });
    EXPECT_NEAR(h.v, 8.0f, 0.001f) << "5 + 30*0.1, not 7 + 30*0.1";
}

// Dragging past an end must not bank the excess: coming back should return to where the cursor says.
TEST(UiWidgetsTest, DragValueClampsWithoutDrifting)
{
    Rig r;
    struct Holder { float v = 5.0f; } h;
    const std::uint64_t idh = make_id("speed").hash;
    r.state.active = idh;

    r.state.drag_x = 0.0f;
    r.frame([&](Ui& u) { drag_value(u, "speed", bind(h, &Holder::v)).range(0, 10).step(0.1f); });
    r.state.drag_x = 5000.0f;
    r.frame([&](Ui& u) { drag_value(u, "speed", bind(h, &Holder::v)).range(0, 10).step(0.1f); });
    EXPECT_FLOAT_EQ(h.v, 10.0f);

    r.state.drag_x = 10.0f;
    r.frame([&](Ui& u) { drag_value(u, "speed", bind(h, &Holder::v)).range(0, 10).step(0.1f); });
    EXPECT_NEAR(h.v, 6.0f, 0.001f) << "clamping must not bank the overshoot";
}

TEST(UiWidgetsTest, DragValueDoesNothingWithoutPointerCapture)
{
    Rig r;
    struct Holder { float v = 5.0f; } h;
    r.state.drag_x = 100.0f;   // a drag, but not on this widget
    r.frame([&](Ui& u) { drag_value(u, "speed", bind(h, &Holder::v)).range(0, 10); });
    EXPECT_FLOAT_EQ(h.v, 5.0f);
}

// --- Combo -----------------------------------------------------------------------------------------

TEST(UiWidgetsTest, ComboOpensOnPressAndSelectsAnOption)
{
    Rig r;
    struct Holder { int v = 0; } h;
    const std::string_view opts[] = { "Low", "Medium", "High" };

    // Closed: only the head is in the tree.
    r.frame([&](Ui& u) { combo(u, "quality", bind(h, &Holder::v)).options(opts); });
    EXPECT_EQ(r.builder.find(make_id("quality.opt1").hash), nullptr);

    // Press the head -> opens.
    r.state.pressed = make_id("quality").hash;
    r.frame([&](Ui& u) { combo(u, "quality", bind(h, &Holder::v)).options(opts); });
    r.state.pressed = 0;
    r.frame([&](Ui& u) { combo(u, "quality", bind(h, &Holder::v)).options(opts); });
    EXPECT_NE(r.builder.find(make_id("quality.opt1").hash), nullptr) << "options must be in the tree";

    // Press an option -> selects it, fires the callback, and closes.
    int seen = -1;
    r.state.pressed = make_id("quality.opt2").hash;
    r.frame([&](Ui& u) {
        combo(u, "quality", bind(h, &Holder::v)).options(opts).on_change([&](int i) { seen = i; });
    });
    EXPECT_EQ(h.v, 2);
    EXPECT_EQ(seen, 2);

    r.state.pressed = 0;
    r.frame([&](Ui& u) { combo(u, "quality", bind(h, &Holder::v)).options(opts); });
    EXPECT_EQ(r.builder.find(make_id("quality.opt1").hash), nullptr) << "selecting must close it";
}

TEST(UiWidgetsTest, ComboWithAnOutOfRangeIndexStillRenders)
{
    Rig r;
    struct Holder { int v = 99; } h;
    const std::string_view opts[] = { "Low", "High" };
    r.frame([&](Ui& u) { combo(u, "quality", bind(h, &Holder::v)).options(opts); });
    EXPECT_NE(r.builder.find(make_id("quality").hash), nullptr);
}

// --- Conventions -----------------------------------------------------------------------------------

// Two widgets sharing a label must still have distinct identity — the brief-05 collision class.
TEST(UiWidgetsTest, NameIsIdentityNotLabel)
{
    Rig r;
    struct Holder { bool a = false; bool b = false; } h;
    r.frame([&](Ui& u) {
        checkbox(u, "cull_a", bind(h, &Holder::a)).label("Cull");
        checkbox(u, "cull_b", bind(h, &Holder::b)).label("Cull");
    });
    EXPECT_NE(r.builder.find(make_id("cull_a").hash), nullptr);
    EXPECT_NE(r.builder.find(make_id("cull_b").hash), nullptr);
}

// An invalid binding must be inert rather than crash — a widget wired to a dead handle is a
// programming error we should survive long enough to see on screen.
TEST(UiWidgetsTest, WidgetsWithInvalidBindingsEmitNothing)
{
    Rig r;
    r.frame([&](Ui& u) {
        checkbox(u, "dead", binding<bool>{});
        drag_value(u, "dead2", binding<float>{});
        combo(u, "dead3", binding<int>{});
    });
    EXPECT_EQ(r.builder.find(make_id("dead").hash), nullptr);
    EXPECT_EQ(r.builder.find(make_id("dead2").hash), nullptr);
}

// --- Slider (absolute) ---------------------------------------------------------------------------
// The difference from DragValue is entirely that a slider maps the cursor onto its OWN rect, which
// it gets from `interaction::hovered_box` — the box the hit test already found.

namespace
{
// Simulates the pointer being captured by the slider's track at a given box and cursor position.
void press_track(Rig& r, std::uint64_t idh, bounding_box box, float cursor_x)
{
    r.state.active = idh;
    r.state.hovered = idh;
    r.state.hovered_box = box;
    r.state.cursor_x = cursor_x;
}
}  // namespace

TEST(UiWidgetsTest, SliderJumpsToWhereTheCursorIs)
{
    Rig r;
    struct Holder { float v = 0.0f; } h;
    const bounding_box track{ 100, 50, { 160, 10 } };   // 160 wide, handle is 14

    // The value maps across the FULL track width now that the fill's own end is the handle.
    press_track(r, make_id("vol").hash, track, 100.0f + 0.8f * 160.0f);
    r.frame([&](Ui& u) { slider(u, "vol", bind(h, &Holder::v)).range(0, 100).width(160); });
    EXPECT_NEAR(h.v, 80.0f, 1.0f);
}

TEST(UiWidgetsTest, SliderReachesBothEnds)
{
    Rig r;
    struct Holder { float v = 50.0f; } h;
    const bounding_box track{ 100, 50, { 160, 10 } };

    press_track(r, make_id("vol").hash, track, 0.0f);   // far left of the track
    r.frame([&](Ui& u) { slider(u, "vol", bind(h, &Holder::v)).range(0, 100).width(160); });
    EXPECT_FLOAT_EQ(h.v, 0.0f);

    press_track(r, make_id("vol").hash, track, 10000.0f);
    r.frame([&](Ui& u) { slider(u, "vol", bind(h, &Holder::v)).range(0, 100).width(160); });
    EXPECT_FLOAT_EQ(h.v, 100.0f);
}

TEST(UiWidgetsTest, SliderIgnoresTheCursorWithoutCapture)
{
    Rig r;
    struct Holder { float v = 25.0f; } h;
    r.state.cursor_x = 9999.0f;   // cursor far away, but the slider does not own the pointer
    r.frame([&](Ui& u) { slider(u, "vol", bind(h, &Holder::v)).range(0, 100).width(160); });
    EXPECT_FLOAT_EQ(h.v, 25.0f);
}

// The TRACK must carry the id, not the row: `hovered_box` is whatever was hit, so if the row owned
// the id the box would include the label and readout and the value would map to the wrong span.
TEST(UiWidgetsTest, TheTrackCarriesTheSliderId)
{
    Rig r;
    struct Holder { float v = 0.0f; } h;
    r.frame([&](Ui& u) { slider(u, "vol", bind(h, &Holder::v)).range(0, 100).width(160); });
    const layout_node* track = r.builder.find(make_id("vol").hash);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->box.dimension.width, 160) << "the id must be on the track, not a wider row";
}

// The control's footprint must not change with its value — an earlier drag-value sized a bar to the
// value inside a fit() row, so the whole widget grew and shrank as it was dragged.
TEST(UiWidgetsTest, WidgetFootprintDoesNotChangeWithValue)
{
    Rig r;
    struct Holder { float v = 0.0f; } h;
    r.frame([&](Ui& u) { drag_value(u, "d", bind(h, &Holder::v)).range(0, 100); });
    const auto narrow = r.builder.find(make_id("d").hash)->box.dimension.width;

    h.v = 100.0f;
    r.frame([&](Ui& u) { drag_value(u, "d", bind(h, &Holder::v)).range(0, 100); });
    const auto wide = r.builder.find(make_id("d").hash)->box.dimension.width;
    EXPECT_EQ(narrow, wide);
}

// --- Text field ------------------------------------------------------------------------------------

TEST(UiWidgetsTest, TextFieldAppendsTypedTextOnlyWhenFocused)
{
    Rig r;
    struct Holder { std::string v; } h;

    // Not focused: keystrokes are ignored, which is the whole reason focus is arbitrated centrally.
    r.state.typed_text = "abc";
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &Holder::v)); });
    EXPECT_EQ(h.v, "");

    r.state.focused = make_id("name").hash;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &Holder::v)); });
    EXPECT_EQ(h.v, "abc");
}

TEST(UiWidgetsTest, TextFieldBackspaceRemovesAWholeCodepoint)
{
    Rig r;
    struct Holder { std::string v = "ab\xC3\xA9"; } h;   // "abé", the last char is 2 bytes
    r.state.focused = make_id("name").hash;
    r.state.backspace = true;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &Holder::v)); });
    // A byte-wise erase would leave a dangling lead byte and render garbage.
    EXPECT_EQ(h.v, "ab");
}

TEST(UiWidgetsTest, TextFieldBackspaceOnEmptyIsSafe)
{
    Rig r;
    struct Holder { std::string v; } h;
    r.state.focused = make_id("name").hash;
    r.state.backspace = true;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &Holder::v)); });
    EXPECT_EQ(h.v, "");
}

TEST(UiWidgetsTest, TextFieldSubmitFiresWithoutChangingTheValue)
{
    Rig r;
    struct Holder { std::string v = "hello"; } h;
    std::string got;
    r.state.focused = make_id("name").hash;
    r.state.submit = true;
    r.frame([&](Ui& u) {
        text_field(u, "name", bind(h, &Holder::v)).on_submit([&](const std::string& s) { got = s; });
    });
    EXPECT_EQ(got, "hello");
    EXPECT_EQ(h.v, "hello");
}

// A focused field must ask for capture, or the same keystrokes also drive the camera.
TEST(UiWidgetsTest, FocusedTextFieldRequestsTextCapture)
{
    Rig r;
    struct Holder { std::string v; } h;

    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &Holder::v)); });
    EXPECT_FALSE(r.ui.wants_text_capture());

    r.state.focused = make_id("name").hash;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &Holder::v)); });
    EXPECT_TRUE(r.ui.wants_text_capture());

    // And it must DROP when focus leaves, or capture would stick on forever.
    r.state.focused = 0;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &Holder::v)); });
    EXPECT_FALSE(r.ui.wants_text_capture());
}

// Two fields, one focused: exactly one consumes the keystrokes.
TEST(UiWidgetsTest, OnlyTheFocusedFieldOfTwoConsumesInput)
{
    Rig r;
    struct Holder { std::string a; std::string b; } h;
    r.state.typed_text = "x";
    r.state.focused = make_id("second").hash;
    r.frame([&](Ui& u) {
        text_field(u, "first", bind(h, &Holder::a));
        text_field(u, "second", bind(h, &Holder::b));
    });
    EXPECT_EQ(h.a, "");
    EXPECT_EQ(h.b, "x");
}

TEST(UiWidgetsTest, TextFieldShowsPlaceholderOnlyWhenEmpty)
{
    Rig r;
    struct Holder { std::string v; } h;
    r.frame([&](Ui& u) {
        text_field(u, "name", bind(h, &Holder::v)).placeholder("type here");
    });
    const std::string empty_dump = dump_layout(r.builder);
    EXPECT_NE(empty_dump.find("type here"), std::string::npos);

    h.v = "abc";
    r.frame([&](Ui& u) {
        text_field(u, "name", bind(h, &Holder::v)).placeholder("type here");
    });
    const std::string filled = dump_layout(r.builder);
    EXPECT_EQ(filled.find("type here"), std::string::npos);
    EXPECT_NE(filled.find("abc"), std::string::npos);
}

// --- Collapsible -----------------------------------------------------------------------------------

// A folded section must not emit its body at all. In a debug UI that is the whole point: a closed
// section should cost nothing, not cost everything except the draw.
TEST(UiWidgetsTest, ClosedCollapsibleDoesNotEmitItsBody)
{
    Rig r;
    bool body_ran = false;
    r.frame([&](Ui& u) {
        collapsible(u, "sec").label("Section").open(false).content([&](Ui& u2) {
            body_ran = true;
            u2.element("inner").fixed(10, 10);
        });
    });
    EXPECT_FALSE(body_ran);
    EXPECT_EQ(r.builder.find(make_id("inner").hash), nullptr);
}

TEST(UiWidgetsTest, CollapsibleTogglesOnPressAndPersists)
{
    Rig r;
    auto author = [&](Ui& u) {
        collapsible(u, "sec").label("Section").open(false).content([](Ui& u2) {
            u2.element("inner").fixed(10, 10);
        });
    };

    r.state.pressed = make_id("sec").hash;
    r.frame(author);
    EXPECT_NE(r.builder.find(make_id("inner").hash), nullptr) << "opens on the frame it is clicked";

    // The open state must survive the rebuild without the author holding it.
    r.state.pressed = 0;
    r.frame(author);
    EXPECT_NE(r.builder.find(make_id("inner").hash), nullptr);

    r.state.pressed = make_id("sec").hash;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("inner").hash), nullptr) << "closes again";
}

TEST(UiWidgetsTest, CollapsibleReportsWhetherItEmitted)
{
    Rig r;
    bool reported = true;
    r.frame([&](Ui& u) {
        reported = collapsible(u, "sec").open(false).content([](Ui&) {});
    });
    EXPECT_FALSE(reported);
}

// --- Tabs ------------------------------------------------------------------------------------------

TEST(UiWidgetsTest, TabsSelectOnClickAndFireOnChange)
{
    Rig r;
    struct Holder { int v = 0; } h;
    const std::string_view labels[] = { "One", "Two", "Three" };
    int seen = -1;

    r.state.pressed = make_id("view.tab2").hash;
    r.frame([&](Ui& u) {
        tabs(u, "view", bind(h, &Holder::v))
            .options(labels)
            .on_change([&](int i) { seen = i; })
            .content([](Ui&) {});
    });
    EXPECT_EQ(h.v, 2);
    EXPECT_EQ(seen, 2);
}

// The bar is emitted before the body, so a click switches the content on the SAME frame rather than
// leaving the previous tab's body up for one more.
TEST(UiWidgetsTest, TabsBodyReflectsTheNewSelectionImmediately)
{
    Rig r;
    struct Holder { int v = 0; } h;
    const std::string_view labels[] = { "One", "Two" };

    r.state.pressed = make_id("view.tab1").hash;
    r.frame([&](Ui& u) {
        tabs(u, "view", bind(h, &Holder::v)).options(labels).content([&](Ui& u2) {
            u2.element(h.v == 1 ? "second_body" : "first_body").fixed(10, 10);
        });
    });
    EXPECT_NE(r.builder.find(make_id("second_body").hash), nullptr);
    EXPECT_EQ(r.builder.find(make_id("first_body").hash), nullptr);
}

TEST(UiWidgetsTest, ClickingTheAlreadySelectedTabDoesNotFireOnChange)
{
    Rig r;
    struct Holder { int v = 1; } h;
    const std::string_view labels[] = { "One", "Two" };
    bool fired = false;
    r.state.pressed = make_id("view.tab1").hash;
    r.frame([&](Ui& u) {
        tabs(u, "view", bind(h, &Holder::v)).options(labels).on_change([&](int) { fired = true; });
    });
    EXPECT_FALSE(fired);
}

// --- Colour picker ---------------------------------------------------------------------------------

TEST(UiWidgetsTest, ColorPickerChannelSlidersWriteBackThroughTheBinding)
{
    Rig r;
    struct Holder { color v{ 10, 20, 30, 255 }; } h;
    const bounding_box track{ 100, 50, { 140, 12 } };

    // Drive the GREEN channel's slider to its maximum.
    press_track(r, make_id("tint.g").hash, track, 10000.0f);
    r.frame([&](Ui& u) { color_picker(u, "tint", bind(h, &Holder::v)).label("Tint"); });

    EXPECT_EQ(h.v.g, 255);
    EXPECT_EQ(h.v.r, 10) << "other channels must be untouched";
    EXPECT_EQ(h.v.b, 30);
}

TEST(UiWidgetsTest, ColorPickerShowsAlphaOnlyWhenAsked)
{
    Rig r;
    struct Holder { color v{ 1, 2, 3, 4 }; } h;

    r.frame([&](Ui& u) { color_picker(u, "tint", bind(h, &Holder::v)); });
    EXPECT_EQ(r.builder.find(make_id("tint.a").hash), nullptr);

    r.frame([&](Ui& u) { color_picker(u, "tint", bind(h, &Holder::v)).alpha(true); });
    EXPECT_NE(r.builder.find(make_id("tint.a").hash), nullptr);
}

// The swatch is SPLIT: opaque hue on one side, actual alpha over a backing on the other. Showing
// only the opaque colour makes the alpha slider look broken (it did); showing only the blended one
// makes the hue unreadable exactly when alpha is low.
TEST(UiWidgetsTest, ColorPickerSwatchShowsBothOpaqueHueAndActualAlpha)
{
    Rig r;
    struct Holder { color v{ 200, 100, 50, 0x40 }; } h;
    r.frame([&](Ui& u) { color_picker(u, "tint", bind(h, &Holder::v)); });

    const std::string d = dump_layout(r.builder);
    EXPECT_NE(d.find("c86432ff"), std::string::npos) << "opaque half missing";
    EXPECT_NE(d.find("c8643240"), std::string::npos) << "alpha half must carry the REAL alpha";
}

// --- Table -----------------------------------------------------------------------------------------

namespace
{
constexpr table_column kCols[] = { { "Name", 100 }, { "Value", 80 } };
}  // namespace

// THE claim virtualization makes: a huge table costs the same as a small one. If this regresses, the
// only symptom is a slow frame, which is exactly the kind of thing nobody notices until it is bad.
TEST(UiWidgetsTest, TableOnlyBuildsVisibleRows)
{
    Rig r;
    int rendered = 0;
    r.frame([&](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(10).rows(
            10000, [&](Ui& u2, std::size_t, std::size_t) {
                ++rendered;
                u2.text("x");
            });
    });
    EXPECT_EQ(rendered, 10 * 2) << "10 rows x 2 columns, not 10000";
}

TEST(UiWidgetsTest, TableRendersFewerRowsThanTheBudgetWhenDataIsShort)
{
    Rig r;
    int rows_seen = 0;
    r.frame([&](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(10).rows(
            3, [&](Ui& u2, std::size_t, std::size_t col) {
                if (col == 0) ++rows_seen;
                u2.text("x");
            });
    });
    EXPECT_EQ(rows_seen, 3);
}

TEST(UiWidgetsTest, TableRowIdentityFollowsTheDataRowNotTheSlot)
{
    Rig r;
    auto author = [&](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(5).rows(
            100, [](Ui& u2, std::size_t, std::size_t) { u2.text("x"); });
    };
    r.frame(author);
    // Row 0 is on screen; row 50 is not.
    EXPECT_NE(r.builder.find(make_id("t.r0").hash), nullptr);
    EXPECT_EQ(r.builder.find(make_id("t.r50").hash), nullptr);
}

TEST(UiWidgetsTest, TableScrollbarDragMovesTheWindow)
{
    Rig r;
    const bounding_box bar{ 200, 40, { 10, 110 } };
    auto author = [&](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(5).rows(
            100, [](Ui& u2, std::size_t, std::size_t) { u2.text("x"); });
    };
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("t.r0").hash), nullptr ? nullptr : r.builder.find(make_id("t.r0").hash));

    // Grab the scrollbar and drag to the middle.
    r.state.active = make_id("t.scroll").hash;
    r.state.hovered = r.state.active;
    r.state.hovered_box = bar;
    r.state.cursor_y = 40.0f + 55.0f;   // halfway down the track
    r.frame(author);

    // The window has moved off the top: row 0 is gone, something mid-table is present.
    EXPECT_EQ(r.builder.find(make_id("t.r0").hash), nullptr);
    EXPECT_NE(r.builder.find(make_id("t.r47").hash), nullptr);
}

TEST(UiWidgetsTest, TableScrollIsClampedToTheEnd)
{
    Rig r;
    const bounding_box bar{ 200, 40, { 10, 110 } };
    auto author = [&](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(5).rows(
            20, [](Ui& u2, std::size_t, std::size_t) { u2.text("x"); });
    };
    r.frame(author);
    r.state.active = make_id("t.scroll").hash;
    r.state.hovered = r.state.active;
    r.state.hovered_box = bar;
    r.state.cursor_y = 10000.0f;
    r.frame(author);

    // 20 rows, 5 visible: the last window starts at 15 and must not run past the data.
    EXPECT_NE(r.builder.find(make_id("t.r19").hash), nullptr);
    EXPECT_EQ(r.builder.find(make_id("t.r20").hash), nullptr);
}

// Sorting is REPORTED, not performed: the table owns no data. Clicking a header tells the author
// which column and direction; clicking the same header again flips it.
TEST(UiWidgetsTest, TableReportsSortColumnAndFlipsDirection)
{
    Rig r;
    std::size_t col = 99;
    bool asc = false;
    int calls = 0;
    auto author = [&](Ui& u) {
        table(u, "t")
            .columns(kCols)
            .visible_rows(3)
            .on_sort([&](std::size_t c, bool a) { col = c; asc = a; ++calls; })
            .rows(10, [](Ui& u2, std::size_t, std::size_t) { u2.text("x"); });
    };

    r.state.pressed = make_id("t.h1").hash;
    r.frame(author);
    EXPECT_EQ(col, 1u);
    EXPECT_TRUE(asc);

    r.frame(author);   // same header again -> descending
    EXPECT_EQ(col, 1u);
    EXPECT_FALSE(asc);
    EXPECT_EQ(calls, 2);
}

TEST(UiWidgetsTest, TableSelectionRoundTripsThroughItsBinding)
{
    Rig r;
    struct Holder { int sel = -1; } h;
    r.state.pressed = make_id("t.r2").hash;
    r.frame([&](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(5).selected(bind(h, &Holder::sel)).rows(
            10, [](Ui& u2, std::size_t, std::size_t) { u2.text("x"); });
    });
    EXPECT_EQ(h.sel, 2);
}

TEST(UiWidgetsTest, TableColumnResizePersistsAndUsesACapturedOrigin)
{
    Rig r;
    auto author = [&](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(3).rows(
            5, [](Ui& u2, std::size_t, std::size_t) { u2.text("x"); });
    };
    r.frame(author);
    const auto before = r.builder.find(make_id("t.h0").hash)->box.dimension.width;
    EXPECT_EQ(before, 100);

    r.state.active = make_id("t.g0").hash;
    r.state.drag_x = 0.0f;
    r.frame(author);          // press frame captures the origin
    r.state.drag_x = 40.0f;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("t.h0").hash)->box.dimension.width, 140);

    // Absolute-from-origin, not accumulated.
    r.state.drag_x = 60.0f;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("t.h0").hash)->box.dimension.width, 160);

    // And it survives the drag ending.
    r.state.active = 0;
    r.state.drag_x = 0.0f;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("t.h0").hash)->box.dimension.width, 160);
}

// --- Tree ------------------------------------------------------------------------------------------

namespace
{
// A synthetic 3-level tree: key encodes the path so children are derivable without storage.
// root k in 1..3, child of k is k*10+i, grandchild k*10+i -> that*10+j.
std::size_t tree_child_count(std::uint64_t key)
{
    return key < 100 ? 3 : 0;   // roots and their children have 3 each; depth 3 are leaves
}
std::uint64_t tree_child(std::uint64_t key, std::size_t i)
{
    return key * 10 + static_cast<std::uint64_t>(i) + 1;
}
constexpr std::uint64_t kRoots[] = { 1, 2, 3 };

Tree& configure(Tree& t)
{
    return t.roots(kRoots).children(tree_child_count, tree_child).visible_rows(20);
}
}  // namespace

// A folded tree costs its roots. This is the property that makes a tree over a large hierarchy
// viable at all — if collapsed subtrees were walked, the "virtualized" claim would be hollow.
TEST(UiWidgetsTest, CollapsedTreeOnlyBuildsRoots)
{
    Rig r;
    int rendered = 0;
    r.frame([&](Ui& u) {
        auto t = tree(u, "tr");
        configure(t).nodes([&](Ui& u2, std::uint64_t, std::size_t) {
            ++rendered;
            u2.text("n");
        });
    });
    EXPECT_EQ(rendered, 3);
    EXPECT_NE(r.builder.find(make_id("tr.n1").hash), nullptr);
    EXPECT_EQ(r.builder.find(make_id("tr.n11").hash), nullptr) << "children must not be walked";
}

TEST(UiWidgetsTest, ExpandingANodeRevealsItsChildrenAndPersists)
{
    Rig r;
    auto author = [&](Ui& u) {
        auto t = tree(u, "tr");
        configure(t).nodes([](Ui& u2, std::uint64_t, std::size_t) { u2.text("n"); });
    };

    r.state.pressed = make_id("tr.x1").hash;   // the expander of root 1
    r.frame(author);
    EXPECT_NE(r.builder.find(make_id("tr.n11").hash), nullptr);

    // Expand state must survive the rebuild without the author holding it.
    r.state.pressed = 0;
    r.frame(author);
    EXPECT_NE(r.builder.find(make_id("tr.n12").hash), nullptr);

    r.state.pressed = make_id("tr.x1").hash;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("tr.n11").hash), nullptr) << "collapses again";
}

// Expanding must not shift which node is which: state is keyed by the author's key, not by position.
TEST(UiWidgetsTest, ExpandStateFollowsTheKeyNotThePosition)
{
    Rig r;
    auto author = [&](Ui& u) {
        auto t = tree(u, "tr");
        configure(t).nodes([](Ui& u2, std::uint64_t, std::size_t) { u2.text("n"); });
    };

    // Open root 3 (last), then open root 1 (which pushes root 3 down the list).
    r.state.pressed = make_id("tr.x3").hash;
    r.frame(author);
    r.state.pressed = make_id("tr.x1").hash;
    r.frame(author);
    r.state.pressed = 0;
    r.frame(author);

    // Both must still be open, despite root 3 having moved.
    EXPECT_NE(r.builder.find(make_id("tr.n11").hash), nullptr);
    EXPECT_NE(r.builder.find(make_id("tr.n31").hash), nullptr);
}

// The expander and the row are separate targets: looking inside a node must not select it.
TEST(UiWidgetsTest, ExpandingDoesNotSelect)
{
    Rig r;
    struct Holder { std::uint64_t sel = 0; } h;
    r.state.pressed = make_id("tr.x1").hash;
    r.frame([&](Ui& u) {
        auto t = tree(u, "tr");
        configure(t).selected(bind(h, &Holder::sel))
                    .nodes([](Ui& u2, std::uint64_t, std::size_t) { u2.text("n"); });
    });
    EXPECT_EQ(h.sel, 0u) << "expanding is not selecting";

    r.state.pressed = make_id("tr.n1").hash;
    r.frame([&](Ui& u) {
        auto t = tree(u, "tr");
        configure(t).selected(bind(h, &Holder::sel))
                    .nodes([](Ui& u2, std::uint64_t, std::size_t) { u2.text("n"); });
    });
    EXPECT_EQ(h.sel, 1u);
}

TEST(UiWidgetsTest, TreeIndentsByDepth)
{
    Rig r;
    r.state.pressed = make_id("tr.x1").hash;
    r.frame([&](Ui& u) {
        auto t = tree(u, "tr");
        configure(t).indent(20).nodes([](Ui& u2, std::uint64_t, std::size_t) { u2.text("n"); });
    });
    const layout_node* root = r.builder.find(make_id("tr.n1").hash);
    const layout_node* kid = r.builder.find(make_id("tr.n11").hash);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(kid, nullptr);

    // The ROWS are full width and start at the same x — deliberately, so a selection highlight spans
    // the whole row. The indent is left padding, so it is the CONTENT that shifts.
    EXPECT_EQ(kid->box.x, root->box.x);
    EXPECT_GT(kid->format.padding.left, root->format.padding.left);

    ASSERT_NE(root->first_child, layout_node::none);
    ASSERT_NE(kid->first_child, layout_node::none);
    EXPECT_GT(r.builder.nodes()[kid->first_child].box.x,
              r.builder.nodes()[root->first_child].box.x)
        << "a child row's content must sit further right than its parent's";
}

TEST(UiWidgetsTest, TreeVirtualizesTheExpandedSet)
{
    Rig r;
    int rendered = 0;
    // Expand every root, so the flattened set is 3 + 9 = 12, but only 4 rows fit.
    r.frame([&](Ui& u) {
        auto t = tree(u, "tr");
        t.roots(kRoots).children(tree_child_count, tree_child);
        t.set_expanded(1, true);
        t.set_expanded(2, true);
        t.set_expanded(3, true);
        t.visible_rows(4).nodes([&](Ui& u2, std::uint64_t, std::size_t) {
            ++rendered;
            u2.text("n");
        });
    });
    EXPECT_EQ(rendered, 4) << "only the window is built, not the whole expanded set";
}

// A children callback that returns a node as its own child must not hang the frame.
TEST(UiWidgetsTest, TreeSurvivesACyclicChildrenCallback)
{
    Rig r;
    const std::uint64_t roots[] = { 7 };
    int rendered = 0;
    r.frame([&](Ui& u) {
        auto t = tree(u, "tr");
        t.roots(roots)
         .children([](std::uint64_t) { return std::size_t{ 1 }; },
                   [](std::uint64_t k, std::size_t) { return k; })   // its own child, forever
         .visible_rows(5);
        t.set_expanded(7, true);
        t.nodes([&](Ui& u2, std::uint64_t, std::size_t) { ++rendered; u2.text("n"); });
    });
    EXPECT_EQ(rendered, 5) << "bounded work, not a lock-up";
}

// --- Graph -----------------------------------------------------------------------------------------

namespace
{
// A small DAG:  a -> b -> d,  a -> c -> d  (a diamond, so layering has something to get wrong)
constexpr std::uint64_t kG[] = { 1, 2, 3, 4 };
constexpr graph_edge kGE[] = { { 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 } };
}  // namespace

TEST(UiWidgetsTest, GraphLayersNodesByLongestPath)
{
    Rig r;
    r.frame([&](Ui& u) {
        graph(u, "g").size(600, 300).edges(kGE).nodes(kG, [](Ui& u2, std::uint64_t) {
            u2.text("n");
        });
    });

    const layout_node* a = r.builder.find(make_id("g.nd1").hash);
    const layout_node* b = r.builder.find(make_id("g.nd2").hash);
    const layout_node* c = r.builder.find(make_id("g.nd3").hash);
    const layout_node* d = r.builder.find(make_id("g.nd4").hash);
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr); ASSERT_NE(c, nullptr); ASSERT_NE(d, nullptr);

    // Every edge must point forward: a < {b,c} < d.
    EXPECT_LT(a->box.x, b->box.x);
    EXPECT_LT(a->box.x, c->box.x);
    EXPECT_LT(b->box.x, d->box.x);
    EXPECT_LT(c->box.x, d->box.x);
    // b and c are siblings in the same column, stacked.
    EXPECT_EQ(b->box.x, c->box.x);
    EXPECT_NE(b->box.y, c->box.y);
}

// LONGEST path, not shortest: with shortest-path layering d would sit at depth 1 (via a->...->d is
// 2 hops, but a naive relax could place it early) and an edge would run backwards.
TEST(UiWidgetsTest, GraphKeepsEveryEdgePointingForward)
{
    Rig r;
    // A long chain plus a shortcut: 1->2->3->4 and 1->4. Node 4 must land after 3, not beside 2.
    constexpr std::uint64_t keys[] = { 1, 2, 3, 4 };
    constexpr graph_edge es[] = { { 1, 2 }, { 2, 3 }, { 3, 4 }, { 1, 4 } };
    r.frame([&](Ui& u) {
        graph(u, "g").size(800, 200).edges(es).nodes(keys, [](Ui& u2, std::uint64_t) { u2.text("n"); });
    });
    const auto x = [&](const char* n) { return r.builder.find(make_id(n).hash)->box.x; };
    EXPECT_GT(x("g.nd4"), x("g.nd3")) << "the shortcut must not pull node 4 forward";
}

TEST(UiWidgetsTest, GraphCullsNodesOutsideTheCanvas)
{
    Rig r;
    // A tiny canvas: only the first column can possibly fit.
    r.frame([&](Ui& u) {
        graph(u, "g").size(120, 200).edges(kGE).nodes(kG, [](Ui& u2, std::uint64_t) { u2.text("n"); });
    });
    EXPECT_NE(r.builder.find(make_id("g.nd1").hash), nullptr);
    EXPECT_EQ(r.builder.find(make_id("g.nd4").hash), nullptr) << "off-canvas nodes must not be built";
}

TEST(UiWidgetsTest, GraphPanMovesTheContentAndIsClamped)
{
    Rig r;
    auto author = [&](Ui& u) {
        graph(u, "g").size(300, 200).edges(kGE).nodes(kG, [](Ui& u2, std::uint64_t) { u2.text("n"); });
    };
    r.frame(author);
    const auto x0 = r.builder.find(make_id("g.nd1").hash)->box.x;

    // Drag the canvas leftwards to reveal content on the right.
    r.state.active = make_id("g.canvas").hash;
    r.state.drag_x = 0.0f;
    r.frame(author);
    r.state.drag_x = -80.0f;
    r.frame(author);
    const layout_node* a = r.builder.find(make_id("g.nd1").hash);
    if (a != nullptr) EXPECT_LT(a->box.x, x0);

    // Panning far past the content must clamp rather than fling everything away.
    r.state.drag_x = -100000.0f;
    r.frame(author);
    EXPECT_NE(r.builder.find(make_id("g.nd4").hash), nullptr)
        << "the last column must still be reachable after clamping";
}

TEST(UiWidgetsTest, GraphNodeDragPersistsByKey)
{
    Rig r;
    auto author = [&](Ui& u) {
        graph(u, "g").size(600, 300).edges(kGE).nodes(kG, [](Ui& u2, std::uint64_t) { u2.text("n"); });
    };
    r.frame(author);
    const auto y0 = r.builder.find(make_id("g.nd2").hash)->box.y;

    r.state.active = make_id("g.nd2").hash;
    r.state.drag_x = 0.0f;
    r.state.drag_y = 0.0f;
    r.frame(author);
    r.state.drag_y = 30.0f;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("g.nd2").hash)->box.y, y0 + 30);

    // Absolute-from-origin, and it survives the drag ending.
    r.state.drag_y = 50.0f;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("g.nd2").hash)->box.y, y0 + 50);
    r.state.active = 0;
    r.state.drag_y = 0.0f;
    r.frame(author);
    EXPECT_EQ(r.builder.find(make_id("g.nd2").hash)->box.y, y0 + 50);
}

TEST(UiWidgetsTest, GraphSelectionRoundTripsAndHighlightsItsEdges)
{
    Rig r;
    struct Holder { std::uint64_t sel = 0; } h;
    auto author = [&](Ui& u) {
        graph(u, "g").size(600, 300).edges(kGE).selected(bind(h, &Holder::sel))
            .nodes(kG, [](Ui& u2, std::uint64_t) { u2.text("n"); });
    };

    r.frame(author);
    const std::string before = dump_layout(r.builder);

    r.state.pressed = make_id("g.nd2").hash;
    r.frame(author);
    EXPECT_EQ(h.sel, 2u);

    r.state.pressed = 0;
    r.frame(author);
    // Selecting a node repaints its incident edges in the accent colour, so the tree differs.
    EXPECT_NE(dump_layout(r.builder), before);
}

// A cyclic input must terminate. A debug view over live data can absolutely produce one, and the
// layering relaxation would otherwise spin.
TEST(UiWidgetsTest, GraphSurvivesACycle)
{
    Rig r;
    constexpr std::uint64_t keys[] = { 1, 2, 3 };
    constexpr graph_edge es[] = { { 1, 2 }, { 2, 3 }, { 3, 1 } };
    int rendered = 0;
    r.frame([&](Ui& u) {
        graph(u, "g").size(800, 300).edges(es).nodes(keys, [&](Ui& u2, std::uint64_t) {
            ++rendered;
            u2.text("n");
        });
    });
    EXPECT_GT(rendered, 0) << "bounded work, not a spin";
}

// The canvas CLIPS, so content may overhang its bounds in the layout and be cut at draw time. What
// must still hold is that a node entirely off-canvas is never built, and that the clip is declared —
// without it, overhang would escape and paint over whatever sits beside the graph.
TEST(UiWidgetsTest, GraphCullsFullyOffscreenNodesAndDeclaresItsClip)
{
    Rig r;
    // Canvas wide enough for two columns only; the third is off-canvas.
    r.frame([&](Ui& u) {
        graph(u, "g").size(260, 200).node_size(90, 30).spacing(40, 14).edges(kGE)
            .nodes(kG, [](Ui& u2, std::uint64_t) { u2.text("n"); });
    });
    ASSERT_EQ(r.builder.find(make_id("g.nd4").hash), nullptr) << "precondition: node 4 is culled";

    const layout_node* canvas = r.builder.find(make_id("g.canvas").hash);
    ASSERT_NE(canvas, nullptr);
    EXPECT_TRUE(canvas->element.clip)
        << "without a declared clip, overhanging content escapes and paints over its neighbours";
}

// --- Clipping ------------------------------------------------------------------------------------

// Content scrolled out of a clipped view must not stay CLICKABLE. Invisible-but-hittable is the
// worst kind of bug to chase, because there is nothing on screen to point at.
TEST(UiWidgetsTest, ClippedAwayContentIsNotHittable)
{
    Rig r;
    r.frame([&](Ui& u) {
        u.element("view").clip().fixed(100, 40).content([](Ui& u2) {
            // Positioned above the view, i.e. scrolled out of sight.
            u2.element("row").floating(0, -60).local().fixed(100, 20);
        });
    });

    const layout_node* row = r.builder.find(make_id("row").hash);
    ASSERT_NE(row, nullptr) << "it is still laid out, just not visible";

    // A point on the hidden row resolves to nothing inside the view, not to the row.
    const layout_node* hit = hit_test_layered(
        r.builder, static_cast<std::uint16_t>(row->box.x + 5),
        static_cast<std::uint16_t>(std::max(0, row->box.y + 5)));
    if (hit != nullptr)
        EXPECT_NE(hit->element.id.hash, make_id("row").hash) << "clipped-away content took a press";
}

// --- Scroll area ---------------------------------------------------------------------------------

namespace
{
// 400px of content in a 100px viewport, so there is always something to scroll to.
void tall_scroll_area(Ui& u)
{
    scroll_area(u, "sc").size(200, 100).speed(48.0f).content([](Ui& u2) {
        u2.element().fixed(50, 400);
    });
}
}  // namespace

// The extent comes from LAST frame's measured content, so the first frame has nothing to scroll and
// the second does. That is the whole cost of measuring what the layout computes, and it is worth
// pinning: a regression here shows up as a scroll area that never moves.
TEST(UiWidgetsTest, ScrollAreaLearnsItsContentHeightAfterOneFrame)
{
    Rig r;
    r.frame(tall_scroll_area);
    const layout_node* view = r.builder.find(make_id("sc").hash);
    ASSERT_NE(view, nullptr);

    // Wheel DOWN (negative notches, as the platform reports it) over the middle of the viewport.
    r.wheel(-2.0f, view->box.x + 20, view->box.y + 50);
    EXPECT_EQ(r.state.wheel, make_id("sc").hash) << "the wheel must route to the area, not its content";

    r.frame(tall_scroll_area);
    const layout_node* content = r.builder.find(make_id("sc.c").hash);
    const layout_node* view2 = r.builder.find(make_id("sc").hash);
    ASSERT_NE(content, nullptr);
    ASSERT_NE(view2, nullptr);
    EXPECT_EQ(content->box.y, view2->box.y - 96) << "two notches at 48px each, scrolled up out of view";
}

TEST(UiWidgetsTest, ScrollAreaClampsToTheBottomOfItsContent)
{
    Rig r;
    r.frame(tall_scroll_area);
    const layout_node* view = r.builder.find(make_id("sc").hash);
    ASSERT_NE(view, nullptr);
    const int cx = view->box.x + 20;
    const int cy = view->box.y + 50;

    for (int i = 0; i < 40; ++i)   // far past the end
    {
        r.wheel(-2.0f, cx, cy);
        r.frame(tall_scroll_area);
    }
    const layout_node* content = r.builder.find(make_id("sc.c").hash);
    const layout_node* view2 = r.builder.find(make_id("sc").hash);
    ASSERT_NE(content, nullptr);
    ASSERT_NE(view2, nullptr);
    // Content is 400 tall plus 8 of padding, in a 100 viewport: the last 100px is all that shows.
    EXPECT_EQ(content->box.dimension.height, 408u);
    EXPECT_EQ(content->box.y, view2->box.y - 308) << "scrolled exactly to the end, never past it";

    // ...and scrolling back up stops at the top.
    for (int i = 0; i < 40; ++i)
    {
        r.wheel(2.0f, cx, cy);
        r.frame(tall_scroll_area);
    }
    const layout_node* content2 = r.builder.find(make_id("sc.c").hash);
    const layout_node* view3 = r.builder.find(make_id("sc").hash);
    ASSERT_NE(content2, nullptr);
    ASSERT_NE(view3, nullptr);
    EXPECT_EQ(content2->box.y, view3->box.y);
}

// The point of routing the wheel through the tree rather than through hover: the cursor is over a
// ROW, and the thing that must move is the list.
TEST(UiWidgetsTest, WheelRoutesToTheInnermostDeclaredContainer)
{
    Rig r;
    const auto author = [](Ui& u) {
        u.element("outer").wheel().fixed(400, 300).content([](Ui& u2) {
            scroll_area(u2, "sc").size(200, 100).content([](Ui& u3) {
                u3.element("leaf").fixed(50, 400);
            });
        });
    };
    r.frame(author);

    const layout_node* leaf = r.builder.find(make_id("leaf").hash);
    ASSERT_NE(leaf, nullptr);
    r.wheel(-1.0f, leaf->box.x + 5, leaf->box.y + 5);
    EXPECT_EQ(r.state.wheel, make_id("sc").hash)
        << "the innermost wheel container wins, even though the cursor is on a leaf inside it";

    // Outside the inner area but still inside the outer one, the outer takes it.
    const layout_node* outer = r.builder.find(make_id("outer").hash);
    ASSERT_NE(outer, nullptr);
    r.wheel(-1.0f, outer->box.x + 350, outer->box.y + 250);
    EXPECT_EQ(r.state.wheel, make_id("outer").hash);
}

TEST(UiWidgetsTest, WheelGoesNowhereWhenNothingUnderTheCursorWantsIt)
{
    Rig r;
    r.frame([](Ui& u) { u.element("plain").fixed(200, 100); });
    const layout_node* plain = r.builder.find(make_id("plain").hash);
    ASSERT_NE(plain, nullptr);
    r.wheel(-3.0f, plain->box.x + 5, plain->box.y + 5);
    EXPECT_EQ(r.state.wheel, 0u) << "an undeclared element must not silently swallow the wheel";
}

// A virtualized list scrolls in ROWS: its position IS a row index, so a pixel offset would have
// nothing to apply itself to.
TEST(UiWidgetsTest, TableWheelScrollsWholeRows)
{
    Rig r;
    static const table_column kCols[] = { { "A", 60 } };
    const auto author = [](Ui& u) {
        table(u, "t").columns(kCols).visible_rows(4).rows(
            100, [](Ui& u2, std::size_t row, std::size_t) { u2.text(u2.own(std::to_string(row))); });
    };
    r.frame(author);
    const layout_node* outer = r.builder.find(make_id("t").hash);
    ASSERT_NE(outer, nullptr);
    r.wheel(-1.0f, outer->box.x + 5, outer->box.y + 5);
    r.frame(author);

    EXPECT_NE(r.builder.find(make_id("t.r3").hash), nullptr) << "scrolled down by three rows";
    EXPECT_EQ(r.builder.find(make_id("t.r0").hash), nullptr);
}

// --- Graph zoom ----------------------------------------------------------------------------------

TEST(UiWidgetsTest, GraphWheelZoomsTheCanvas)
{
    Rig r;
    const auto author = [](Ui& u) {
        graph(u, "g").size(400, 200).node_size(90, 30).spacing(40, 14).edges(kGE)
            .nodes(kG, [](Ui& u2, std::uint64_t) { u2.text("n"); });
    };
    r.frame(author);
    const layout_node* n1 = r.builder.find(make_id("g.nd1").hash);
    ASSERT_NE(n1, nullptr);
    EXPECT_EQ(n1->box.dimension.width, 90u) << "precondition: unzoomed";

    const layout_node* canvas = r.builder.find(make_id("g.canvas").hash);
    ASSERT_NE(canvas, nullptr);
    // Over the canvas background, away from any node, so this is not also a node hit. Held as plain
    // ints: the next frame clears the builder and every node pointer with it.
    const int cx = canvas->box.x + 380;
    const int cy = canvas->box.y + 180;
    r.wheel(4.0f, cx, cy);
    EXPECT_EQ(r.state.wheel, make_id("g.canvas").hash);

    r.frame(author);
    const layout_node* z1 = r.builder.find(make_id("g.nd1").hash);
    ASSERT_NE(z1, nullptr);
    EXPECT_GT(z1->box.dimension.width, 90u) << "four notches in must make the nodes bigger";

    // ...and back out again, past 1.0, to the floor rather than through it.
    for (int i = 0; i < 40; ++i) { r.wheel(-4.0f, cx, cy); r.frame(author); }
    const layout_node* z2 = r.builder.find(make_id("g.nd1").hash);
    ASSERT_NE(z2, nullptr);
    EXPECT_EQ(z2->box.dimension.width, static_cast<std::uint16_t>(std::lround(90 * 0.35)))
        << "zoom is clamped, so a stuck wheel cannot shrink the graph to nothing";
}

TEST(UiWidgetsTest, ContentInsideTheClipStaysHittable)
{
    Rig r;
    r.frame([&](Ui& u) {
        u.element("view").clip().fixed(100, 40).content([](Ui& u2) {
            u2.element("row").floating(0, 4).local().fixed(100, 20);
        });
    });
    const layout_node* row = r.builder.find(make_id("row").hash);
    ASSERT_NE(row, nullptr);
    const layout_node* hit = hit_test_layered(
        r.builder, static_cast<std::uint16_t>(row->box.x + 5),
        static_cast<std::uint16_t>(row->box.y + 5));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->element.id.hash, make_id("row").hash);
}
