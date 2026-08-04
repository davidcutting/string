#include <gtest/gtest.h>

#include <cmath>

#include <fstream>
#include <iterator>

#include <string/ui/layout_dump.hpp>
#include <string/ui/text_measurer.hpp>
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
        // DEFERRED SURFACES, drained exactly as the host does: a popup is declared during authoring
        // and emitted after layout, so it can anchor to THIS frame's geometry. A rig that skipped
        // this would simply never emit popups, and every menu test would fail for the wrong reason.
        while (ui.emit_next_deferred()) builder.end(screen);
        // The host's post-layout seam: sizes exist only now, and a widget that measures its own
        // content (a scroll area) reads what this caches on the NEXT frame.
        ui.observe();
        ui.end_frame();
        // EVENTS, not state: begin_interaction refills these from each frame's input, so a rig that
        // left them set would deliver the same keystroke every frame — which is exactly how a caret
        // key leaked into a later frame's typing and made a correct widget look broken.
        state.scroll_y = 0.0f;
        state.typed_text = {};
        state.backspace = state.del = false;
        state.caret_left = state.caret_right = false;
        state.caret_home = state.caret_end = false;
        state.copy = state.cut = state.paste = state.select_all = false;
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

    // Re-supplied, because typed text is a per-FRAME event: the host refills it from that frame's
    // input, so a keystroke ignored while unfocused is gone rather than queued.
    r.state.focused = make_id("name").hash;
    r.state.typed_text = "abc";
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
    // Submit is ROUTED now, not a global flag: the router resolved it to this field.
    r.state.action_target[static_cast<std::size_t>(ui_action::submit)] = make_id("name").hash;
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

// --- Menu bar ------------------------------------------------------------------------------------

namespace
{
struct MenuFlags { bool a = false; bool b = false; };

// Two menus; the first has a toggle bound to `f.a`, the second an item that sets `f.b`.
auto menu_author(MenuFlags& f)
{
    return [&f](Ui& u) {
        auto bar = menu_bar(u, "bar");
        bar.menu("View", [&](Menu& m) { m.toggle("Alpha", bind(f, &MenuFlags::a)); });
        bar.menu("Debug", [&](Menu& m) { m.item("Beta", [&] { f.b = true; }); });
    };
}
}  // namespace

TEST(UiWidgetsTest, MenuOpensOnClickAndClosesOnASecond)
{
    Rig r;
    MenuFlags f;
    r.frame(menu_author(f));
    EXPECT_EQ(r.builder.find(make_id("bar.popup").hash), nullptr) << "closed to begin with";

    // Press the first bar button: the menu must be open on the SAME frame, not the next — its items
    // are what gets emitted, so a frame-late open would be visible.
    r.state.pressed = make_id("bar.m0").hash;
    r.frame(menu_author(f));
    EXPECT_NE(r.builder.find(make_id("bar.popup").hash), nullptr);
    EXPECT_NE(r.builder.find(make_id("bar.m0.i0").hash), nullptr) << "its items must exist";

    // Pressing the same button again closes it.
    r.state.pressed = make_id("bar.m0").hash;
    r.frame(menu_author(f));
    EXPECT_EQ(r.builder.find(make_id("bar.popup").hash), nullptr);
}

// The behaviour that makes a menu bar a menu bar rather than a row of combos.
TEST(UiWidgetsTest, HoverSwitchesBetweenOpenMenus)
{
    Rig r;
    MenuFlags f;
    r.state.pressed = make_id("bar.m0").hash;
    r.frame(menu_author(f));
    r.state.pressed = 0;

    // Hovering the OTHER button while one is open switches to it, with no click.
    r.state.hovered = make_id("bar.m1").hash;
    r.frame(menu_author(f));
    EXPECT_NE(r.builder.find(make_id("bar.m1.i0").hash), nullptr) << "menu 1's item";
    EXPECT_EQ(r.builder.find(make_id("bar.m0.i0").hash), nullptr) << "menu 0 must have closed";
}

// ...but only while a menu is already open, or sweeping the bar would spring menus at you.
TEST(UiWidgetsTest, HoverAloneDoesNotOpenAMenu)
{
    Rig r;
    MenuFlags f;
    r.state.hovered = make_id("bar.m1").hash;
    r.frame(menu_author(f));
    EXPECT_EQ(r.builder.find(make_id("bar.popup").hash), nullptr);
}

TEST(UiWidgetsTest, ClickingOutsideDismissesTheMenu)
{
    Rig r;
    MenuFlags f;
    r.state.pressed = make_id("bar.m0").hash;
    r.frame(menu_author(f));
    ASSERT_NE(r.builder.find(make_id("bar.popup").hash), nullptr);

    // A press on something that is neither a bar button nor one of this menu's items.
    r.state.pressed = make_id("something_else").hash;
    r.frame(menu_author(f));
    EXPECT_EQ(r.builder.find(make_id("bar.popup").hash), nullptr);
}

TEST(UiWidgetsTest, MenuItemsDriveTheirBindingAndCallback)
{
    Rig r;
    MenuFlags f;
    r.state.pressed = make_id("bar.m0").hash;
    r.frame(menu_author(f));

    // Press the toggle: it flips on this frame.
    r.state.pressed = make_id("bar.m0.i0").hash;
    r.frame(menu_author(f));
    EXPECT_TRUE(f.a);
    EXPECT_NE(r.builder.find(make_id("bar.popup").hash), nullptr)
        << "still drawn on the click frame — the item's callback fires DURING emission, so closing "
           "before emitting would mean never running it";

    // ...and it is gone on the next.
    r.state.pressed = 0;
    r.frame(menu_author(f));
    EXPECT_EQ(r.builder.find(make_id("bar.popup").hash), nullptr) << "choosing an item closes it";

    // The second menu's plain item fires its callback.
    r.state.pressed = make_id("bar.m1").hash;
    r.frame(menu_author(f));
    r.state.pressed = make_id("bar.m1.i0").hash;
    r.frame(menu_author(f));
    EXPECT_TRUE(f.b);
}

// The anchor is the thing that made this possible at all: `observed_box` supplies the button's
// post-layout rect, which is exactly what `Combo`'s header said did not exist.
TEST(UiWidgetsTest, ThePopupIsAnchoredUnderItsOwnButton)
{
    Rig r;
    MenuFlags f;
    r.state.pressed = make_id("bar.m1").hash;
    r.frame(menu_author(f));
    r.state.pressed = 0;
    r.frame(menu_author(f));   // second frame: the anchor has been observed

    const layout_node* btn = r.builder.find(make_id("bar.m1").hash);
    const layout_node* popup = r.builder.find(make_id("bar.popup").hash);
    ASSERT_NE(btn, nullptr);
    ASSERT_NE(popup, nullptr);
    // SCREEN space on both sides: the popup is its own surface, the button is not, so their boxes
    // are only comparable once each has been resolved through its placement.
    const bounding_box pb = r.builder.screen_box(*popup);
    const bounding_box bb = r.builder.screen_box(*btn);
    EXPECT_EQ(pb.x, bb.x) << "left-aligned with the button that opened it";
    EXPECT_EQ(pb.y, bb.y + bb.dimension.height) << "directly beneath it";
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

// --- Scroll area over a float-local canvas -----------------------------------------------------
//
// Reproduces the brief-14 DAG panel's shape: a scroll area whose content is a fixed-size "canvas"
// element positioning its children with floating(x,y).local(), followed by ordinary flow rows.
//
// Both halves must translate TOGETHER when scrolled. If the canvas's floating children ignored the
// ancestor's scroll offset, the flow rows would slide over a pinned graph — which is what the panel
// looked like in use.
namespace
{
// Author the panel's shape. `rows` is how many flow rows follow the canvas, so a test can grow the
// content the way selecting a pass does.
auto dag_shape(int rows)
{
    return [rows](Ui& u) {
        scroll_area(u, "sc").size(200, 100).content([&](Ui& s) {
            s.element().fixed(180, 150).content([&](Ui& c) {
                c.element().floating(0, 0).local().fixed(20, 20);     // "node" at canvas origin
                c.element().floating(0, 100).local().fixed(20, 20);   // "node" lower down
            });
            for (int i = 0; i < rows; ++i)
                u.text("row").font(12);
        });
    };
}

// The y of the nth id-less fixed 20x20 box — the canvas "nodes".
std::vector<int> node_ys(const layout_builder& b)
{
    std::vector<int> out;
    for (const layout_node& n : b.nodes())
        if (n.box.dimension.width == 20 && n.box.dimension.height == 20) out.push_back(n.box.y);
    return out;
}
}  // namespace

TEST(UiWidgetsTest, ScrollAreaTranslatesFloatLocalChildrenWithTheContent)
{
    Rig r;
    r.frame(dag_shape(0));                       // frame 1: lay out, cache the extent
    const std::vector<int> before = node_ys(r.builder);
    ASSERT_EQ(before.size(), 2u);

    r.wheel(-2.0f, 100, 60);                     // wheel down over the viewport
    r.frame(dag_shape(0));
    const std::vector<int> after = node_ys(r.builder);
    ASSERT_EQ(after.size(), 2u);

    // Both nodes moved UP by the same amount: the canvas is not pinned, and its float-local
    // children inherit the scroll translation rather than resolving against a stale origin.
    EXPECT_LT(after[0], before[0]);
    EXPECT_EQ(before[0] - after[0], before[1] - after[1]);
}

TEST(UiWidgetsTest, ScrollExtentGrowsWithContentAddedBySelection)
{
    Rig r;
    r.frame(dag_shape(0));
    r.frame(dag_shape(0));                       // settle: extent comes from last frame's layout

    // Scroll to the end of the SHORT content, then grow it the way selecting a pass does.
    for (int i = 0; i < 20; ++i) { r.wheel(-2.0f, 100, 60); r.frame(dag_shape(0)); }
    const float short_off = scroll_area(r.ui, "sc").offset();

    r.frame(dag_shape(30));
    for (int i = 0; i < 20; ++i) { r.wheel(-2.0f, 100, 60); r.frame(dag_shape(30)); }
    const float long_off = scroll_area(r.ui, "sc").offset();

    // Taller content must reach further. Equal offsets would mean the extent was measured once and
    // never updated — the failure mode where the bar is stuck at a fixed range.
    EXPECT_GT(long_off, short_off);
}

TEST(UiWidgetsTest, ScrollBarAppearsOnlyWhenThereIsSomethingToScroll)
{
    const auto has_bar = [](const layout_builder& b) {
        for (const layout_node& n : b.nodes())
            if (n.element.id.hash == make_id("sc.scroll").hash) return true;
        return false;
    };

    Rig r;
    // Content shorter than the 100px viewport: nothing to scroll, so no track.
    const auto shrt = [](Ui& u) {
        scroll_area(u, "sc").size(200, 100).content([&](Ui& s) { s.element().fixed(50, 30); });
    };
    r.frame(shrt);
    r.frame(shrt);   // settle: the extent comes from the previous frame's measured content
    EXPECT_FALSE(has_bar(r.builder));

    // Grow past the viewport and the track appears.
    const auto tall = [](Ui& u) {
        scroll_area(u, "sc").size(200, 100).content([&](Ui& s) { s.element().fixed(50, 400); });
    };
    r.frame(tall);
    r.frame(tall);
    EXPECT_TRUE(has_bar(r.builder));

    // ...and goes again when it no longer overflows. The gutter stays reserved throughout, so this
    // is purely the bar appearing and disappearing — the content never reflows.
    r.frame(shrt);
    r.frame(shrt);
    EXPECT_FALSE(has_bar(r.builder));
}

// --- Theme state -> colour -----------------------------------------------------------------------
//
// These mappings used to be ~13 hand-written ternaries scattered through the widgets, so "what does
// hover look like" had thirteen answers that happened to agree. Now there is one, and these pin its
// table — including the two precedence rules, which are the only part a reader could get wrong.

TEST(UiThemeTest, SurfaceMapsStateToFillWithSelectionOutrankingHover)
{
    const Theme th{};
    EXPECT_EQ(th.surface({}).r, th.panel.r);
    EXPECT_EQ(th.surface({ .hot = true }).r, th.panel_alt.r);
    EXPECT_EQ(th.surface({ .active = true }).r, th.panel_alt.r);
    EXPECT_EQ(th.surface({ .selected = true }).r, th.accent.r);
    // Selection outranks hover: a selected row must stay legible as selected while you sweep over it.
    EXPECT_EQ(th.surface({ .hot = true, .selected = true }).r, th.accent.r);
}

TEST(UiThemeTest, OutlineMapsStateToStrokeWithFocusOutrankingHover)
{
    const Theme th{};
    EXPECT_EQ(th.outline({}).r, th.stroke.r);
    EXPECT_EQ(th.outline({ .hot = true }).r, th.stroke_hi.r);
    EXPECT_EQ(th.outline({ .active = true }).r, th.stroke_hi.r);
    EXPECT_EQ(th.outline({ .focused = true }).r, th.accent.r);
    // Focus outranks hover: focus persists and is what the keyboard acts on, so it has to stay
    // readable while the pointer is somewhere else entirely.
    EXPECT_EQ(th.outline({ .hot = true, .focused = true }).r, th.accent.r);
}

// The function being right is not the same as the widgets being WIRED to it. The layout dump gate
// only ever sees a resting frame, so these are the states no capture can check.
TEST(UiThemeTest, WidgetsActuallyDrawTheirHoveredAndSelectedStates)
{
    Rig r;
    const std::uint64_t row1 = make_id("tbl.r1").hash;

    const table_column cols[] = { { "name", 120 } };
    struct Sel { int row = 1; } sel;

    // Hover row 1 while row 1 is also the selection.
    r.state.hovered = row1;
    r.frame([&](Ui& u) {
        table(u, "tbl").columns(cols).visible_rows(4).selected(bind(sel, &Sel::row))
            .rows(3, [](Ui&, std::size_t, std::size_t) {});
    });

    const layout_node* n = r.builder.find(row1);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->element.color.r, r.theme.accent.r) << "selected must outrank hovered on a row";

    // Now hover row 2, which is NOT selected.
    const std::uint64_t row2 = make_id("tbl.r2").hash;
    r.state.hovered = row2;
    r.frame([&](Ui& u) {
        table(u, "tbl").columns(cols).visible_rows(4).selected(bind(sel, &Sel::row))
            .rows(3, [](Ui&, std::size_t, std::size_t) {});
    });
    const layout_node* n2 = r.builder.find(row2);
    ASSERT_NE(n2, nullptr);
    EXPECT_EQ(n2->element.color.r, r.theme.panel_alt.r) << "a hovered unselected row takes panel_alt";
}

// Repro for "Esc doesn't close the console" (user, 2026-08-03): a modal panel declaring cancel,
// authored exactly the way DebugPanels::author_console does it.
TEST(UiThemeTest, ModalPanelReceivesCancelWithNothingFocused)
{
    Rig r;
    r.state.screen = dimension{ 800, 600 };

    // Frame 1: author, then resolve with the action arriving — the real frame order.
    r.frame([&](Ui& u) {
        Panel p = u.panel("dbg_console");
        p.title("Console").modal().handles(ui_action::cancel)
         .initial({ 16, 16, 760, 330 }).content([](Ui& c) { c.text("hi"); });
    });

    interaction_input in;
    in.ui_mode = true;
    in.screen = dimension{ 800, 600 };
    in.cursor_x = -1;
    in.cursor_y = -1;
    in.actions = action_bit(ui_action::cancel);
    resolve_interaction(r.state, in, r.builder);

    EXPECT_TRUE(r.state.took(make_id("dbg_console").hash, ui_action::cancel));

    // Frame 2: the author reads the target resolved at the end of frame 1.
    bool took = false;
    r.frame([&](Ui& u) {
        Panel p = u.panel("dbg_console");
        p.title("Console").modal().handles(ui_action::cancel)
         .initial({ 16, 16, 760, 330 }).content([](Ui& c) { c.text("hi"); });
        took = p.took(ui_action::cancel);
    });
    EXPECT_TRUE(took) << "the panel must see it on the frame after it was routed";
}

// --- TextField caret + selection rendering -------------------------------------------------------
//
// The layout-dump gate cannot see any of this: it captures an unfocused frame, and a caret only
// exists while focused. These drive the widget itself rather than the editing model (which
// text_edit_test covers) — the question here is whether the widget is WIRED to it and whether the
// tree it emits actually shows a caret and a selection.

namespace
{
struct TextHolder { std::string v; };
}  // namespace

TEST(UiWidgetsTest, TextFieldPutsTheCaretAtTheEndWhenItGainsFocus)
{
    Rig r;
    TextHolder h{ "abc" };
    r.state.focused = make_id("name").hash;
    // Typing on the very first focused frame must APPEND, not prepend — a caret left at 0 is what
    // makes a field feel broken the instant you click into it.
    r.state.typed_text = "d";
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });
    EXPECT_EQ(h.v, "abcd");
}

TEST(UiWidgetsTest, TextFieldEmitsACaretOnlyWhileFocused)
{
    Rig r;
    TextHolder h{ "ab" };

    // Unfocused: the text is one run and there is no caret element.
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });
    const std::size_t unfocused_nodes = r.builder.nodes().size();

    r.state.focused = make_id("name").hash;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });
    EXPECT_GT(r.builder.nodes().size(), unfocused_nodes)
        << "a focused field emits a caret the unfocused one does not";
}

// The caret is drawn by SPLITTING the run rather than by measuring it — the kit owns no font atlas,
// so layout has to do the positioning. This is what that looks like in the tree.
TEST(UiWidgetsTest, TextFieldSplitsTheRunAroundTheCaret)
{
    Rig r;
    TextHolder h{ "abcd" };
    r.state.focused = make_id("name").hash;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });

    // Caret at the end after focus: one run before it, none after.
    auto runs = [&] {
        std::vector<std::string_view> out;
        for (const auto& n : r.builder.nodes())
            if (n.element.text != 0)
                out.push_back(r.builder.text_runs()[n.element.text].str);
        return out;
    };
    EXPECT_EQ(runs(), (std::vector<std::string_view>{ "abcd" }));

    // Move left twice: the run must split at the caret, and the two halves must still spell the
    // original — a split that loses or duplicates a character would be invisible in a screenshot.
    for (int i = 0; i < 2; ++i)
    {
        r.state.caret_left = true;
        r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });
    }
    EXPECT_EQ(runs(), (std::vector<std::string_view>{ "ab", "cd" }));
    EXPECT_EQ(h.v, "abcd") << "moving the caret must not touch the text";
}

TEST(UiWidgetsTest, TextFieldSelectionEmitsAHighlightedRun)
{
    Rig r;
    TextHolder h{ "abcd" };
    r.state.focused = make_id("name").hash;
    r.state.select_all = true;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });

    // The whole run is selected, so it is emitted inside a filled container rather than as plain
    // text — that fill IS the highlight, since text nodes draw glyphs and not a rect.
    bool highlighted = false;
    for (const auto& n : r.builder.nodes())
    {
        if (n.element.text == 0 || n.is_root()) continue;
        if (r.builder.text_runs()[n.element.text].str != "abcd") continue;
        const auto& parent = r.builder.nodes()[n.parent];
        if (parent.element.color.a > 0 && parent.element.color.r == r.theme.accent.r)
            highlighted = true;
    }
    EXPECT_TRUE(highlighted);
}

TEST(UiWidgetsTest, TextFieldCopyRequestsAClipboardWrite)
{
    Rig r;
    TextHolder h{ "abcd" };
    r.state.focused = make_id("name").hash;
    r.state.select_all = true;
    r.state.copy = true;
    r.frame([&](Ui& u) {
        text_field(u, "name", bind(h, &TextHolder::v));
        // Read INSIDE the frame: the request is cleared at the next begin_frame, like every other
        // per-frame event.
        EXPECT_EQ(u.clipboard_write(), "abcd");
    });
    EXPECT_EQ(h.v, "abcd") << "a copy must not modify the field";
}

// The console's tab-completion and history write the bound string directly. A caret left at its old
// offset then sits in the middle of the text that just appeared — and `clamp` cannot catch it,
// because the string usually gets LONGER so the stale offset stays valid while being wrong.
TEST(UiWidgetsTest, TextFieldSnapsTheCaretToTheEndWhenTheValueIsReplacedExternally)
{
    Rig r;
    TextHolder h{ "r.ca" };
    r.state.focused = make_id("name").hash;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });

    // Something else completes the line, as tab-completion does.
    h.v = "r.capture.frame ";
    r.state.typed_text = "1";
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });
    EXPECT_EQ(h.v, "r.capture.frame 1") << "typing must continue at the END of the new value";
}

// ...but the widget's OWN edits must not look external, or every keystroke would fling the caret to
// the end and editing mid-string would be impossible.
TEST(UiWidgetsTest, TextFieldOwnEditsDoNotCountAsAnExternalReplacement)
{
    Rig r;
    TextHolder h{ "abcd" };
    r.state.focused = make_id("name").hash;
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });

    for (int i = 0; i < 2; ++i)   // caret to the middle
    {
        r.state.caret_left = true;
        r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });
    }
    r.state.typed_text = "X";
    r.frame([&](Ui& u) { text_field(u, "name", bind(h, &TextHolder::v)); });
    EXPECT_EQ(h.v, "abXcd") << "the insert must land at the caret, not at the end";
}

// --- Popups anchor to THIS frame, not last frame (brief 12b M1) ----------------------------------
//
// The regression this closes: the popup read `observed_box` — last frame's box — so whenever its
// anchor moved, the popup trailed it by a frame. The old comment argued that was acceptable because
// "the bar does not move", which was a stale answer being defended rather than a right one.
//
// MOVING THE ANCHOR IS THE WHOLE TEST, and two earlier drafts failed to actually move it:
//   * wrapping the bar in an offset parent does nothing — `MenuBar` pins itself at `.floating(0, 0)`
//     in ROOT space. That draft passed identically with the stale read, i.e. proved nothing.
//   * widening a label does nothing under the shared Rig, which lays out with `no_measure`: every
//     label is zero-wide there, so buttons sit at fixed offsets whatever they say.
// So this test brings a REAL measurer, which is what makes a label's width move the button after it.
TEST(UiWidgetsTest, AnOpenPopupTracksItsAnchorWithinTheSameFrame)
{
    std::ifstream f(STRING_FONT_TEST_PATH, std::ios::binary);
    const std::vector<std::uint8_t> ttf{ std::istreambuf_iterator<char>(f),
                                         std::istreambuf_iterator<char>() };
    ASSERT_FALSE(ttf.empty()) << "test font not found at " << STRING_FONT_TEST_PATH;
    dynamic_font_atlas atlas(ttf);

    layout_builder builder;
    interaction state{};
    state.screen = dimension{ 800, 600 };
    Motion motion;
    Theme theme{};
    PanelStore panels;
    Ui ui{ builder, state, motion, theme, panels };

    const std::uint64_t btn1 = make_id("bar.m1").hash;

    // The host's frame shape, with measurement: author, lay out, then drain deferred surfaces —
    // each closed with the same measurer, which is what lets a popup read a real anchor box.
    const auto frame = [&](std::string_view first_label) {
        ui.begin_frame();
        builder.clear();
        builder.begin(format{ .direction = direction::VERTICAL });
        menu_bar(ui, "bar")
            .menu(first_label, [](Menu& m) { m.item("A", [] {}); })
            .menu("Edit", [](Menu& m) { m.item("Open", [] {}); });
        const auto measure = [&] {
            return dynamic_text_measurer{ &atlas, builder.text_runs() };
        };
        builder.end(state.screen, measure());
        while (ui.emit_next_deferred()) builder.end(state.screen, measure());
        ui.observe();
        ui.end_frame();
    };

    frame("File");
    state.pressed = btn1;                 // open the SECOND menu
    frame("File");
    state.pressed = 0;

    const layout_node* before = builder.find(btn1);
    ASSERT_NE(before, nullptr);
    const int x_before = builder.screen_box(*before).x;

    // Widen the first label: every button after it shifts right THIS frame.
    frame("Fileeeeeeeeeeeeeeee");

    const layout_node* button = builder.find(btn1);
    const layout_node* popup = builder.find(make_id("bar.popup").hash);
    ASSERT_NE(button, nullptr);
    ASSERT_NE(popup, nullptr) << "the menu should still be open";
    const int button_x = builder.screen_box(*button).x;
    const int popup_x = builder.screen_box(*popup).x;
    ASSERT_NE(button_x, x_before) << "the anchor did not actually move — the test proves nothing";

    EXPECT_EQ(popup_x, button_x)
        << "popup at x=" << popup_x << " but its button is at x=" << button_x
        << " (was " << x_before << ") — the anchor came from the previous frame";
    const bounding_box button_box = builder.screen_box(*button);
    EXPECT_EQ(builder.screen_box(*popup).y, button_box.y + button_box.dimension.height);
}
