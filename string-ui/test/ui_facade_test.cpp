#include <gtest/gtest.h>

#include <string>

#include <string/ui/layout.hpp>
#include <string/ui/layout_dump.hpp>
#include <string/ui/ui.hpp>

using namespace string;
using namespace string::ui;

namespace
{
// A minimal stand-in for a handle-backed target that is NOT a CVar. Its whole job is to prove
// `bind()` is handle-GENERIC: if the accessor were CVar-shaped, brief 15's material/light store
// would have to retrofit it. This is the shape that store will have.
struct FakeStoreHandle
{
    float v = 0.0f;
    [[nodiscard]] float get() const { return v; }
    void set(float nv) { v = nv; }
};

struct Sun
{
    float intensity = 1.0f;
};

// Test rig: the four collaborators a Ui needs, none of which touch a GPU.
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

// --- Identity ------------------------------------------------------------------------------------

// Names are IDENTITY, independent of the visible label. This is the fix for brief 05's silent
// id-collision class: two elements showing the same text still have distinct stable ids.
TEST(UiFacadeTest, NameIsIdentityAndIndependentOfLabel)
{
    Rig r;
    r.ui.begin_frame();
    {
        // Held by value (the factory returns a prvalue), then configured — the modifiers return a
        // reference, so chaining them into an `Element` variable would be a copy, which is deleted.
        Element a = r.ui.element("hp_value");
        Element b = r.ui.element("mp_value");
        a.text("100");
        b.text("100");
        EXPECT_NE(a.id_hash(), b.id_hash());
        EXPECT_EQ(a.id_hash(), make_id("hp_value").hash);
    }
    r.ui.end_frame();
}

// --- Nesting -------------------------------------------------------------------------------------

TEST(UiFacadeTest, ContentClosureNestsChildren)
{
    Rig r;
    r.ui.begin_frame();
    r.ui.element("root").themed().content([](Ui& u) {
        u.element("a").text("one");
        u.element("b").text("two");
    });
    r.builder.end(dimension{ 400, 300 });
    r.ui.end_frame();

    ASSERT_EQ(r.builder.nodes().size(), 3u);           // panel + 2 labels
    const layout_node* panel = r.builder.find(make_id("root").hash);
    ASSERT_NE(panel, nullptr);
    EXPECT_FALSE(panel->is_leaf());
    EXPECT_NE(r.builder.find(make_id("a").hash), nullptr);
    EXPECT_NE(r.builder.find(make_id("b").hash), nullptr);
}

// An element with no .content() emits as a leaf when it goes out of scope — the author never has to
// remember to close anything.
TEST(UiFacadeTest, LeafEmitsOnScopeExit)
{
    Rig r;
    r.ui.begin_frame();
    r.ui.element("root").themed().content([](Ui& u) { u.element("leaf").fixed(10, 10); });
    r.builder.end(dimension{ 100, 100 });
    r.ui.end_frame();
    EXPECT_NE(r.builder.find(make_id("leaf").hash), nullptr);
}

// --- The scratch arena ---------------------------------------------------------------------------

// The bug class this kills: add_text takes a NON-OWNING view, so a caller passing a temporary used
// to dangle. The facade copies into a frame arena, so a temporary is safe by construction.
TEST(UiFacadeTest, LabelSurvivesATemporaryString)
{
    Rig r;
    r.ui.begin_frame();
    r.ui.element("root").themed().content([](Ui& u) {
        for (int i = 0; i < 64; ++i)
            u.element(u.own("row" + std::to_string(i))).text("frame " + std::to_string(i));
    });
    r.builder.end(dimension{ 400, 900 });

    // Every view must still read back correctly AFTER all the temporaries died. A vector-backed
    // arena would have relocated the early SSO buffers and corrupted these.
    const std::span<const text_run> texts = r.builder.text_runs();
    ASSERT_GE(texts.size(), 65u);
    EXPECT_EQ(texts[1].str, "frame 0");
    EXPECT_EQ(texts[64].str, "frame 63");
    r.ui.end_frame();
}

TEST(UiFacadeTest, ArenaIsClearedEachFrame)
{
    Rig r;
    r.ui.begin_frame();
    const std::string_view a = r.ui.own(std::string("first"));
    EXPECT_EQ(a, "first");
    r.ui.end_frame();

    r.ui.begin_frame();               // clears — last frame's views are dead by design
    const std::string_view b = r.ui.own(std::string("second"));
    EXPECT_EQ(b, "second");
    r.ui.end_frame();
}

// --- Events --------------------------------------------------------------------------------------

// Callbacks are the primitive; poll is sugar over the SAME resolved state, not a parallel path.
TEST(UiFacadeTest, CallbackAndPollAgree)
{
    Rig r;
    r.state.pressed = make_id("go").hash;
    r.ui.begin_frame();

    bool fired = false;
    Element e = r.ui.button("go");
    e.on_click([&] { fired = true; });
    EXPECT_TRUE(fired);
    EXPECT_TRUE(e.clicked());

    Element other = r.ui.button("cancel");
    bool other_fired = false;
    other.on_click([&] { other_fired = true; });
    EXPECT_FALSE(other_fired);
    EXPECT_FALSE(other.clicked());
    r.ui.end_frame();
}

TEST(UiFacadeTest, PollsReflectInteractionState)
{
    Rig r;
    r.state.hovered = make_id("h").hash;
    r.state.focused = make_id("f").hash;
    r.state.active = make_id("d").hash;
    r.ui.begin_frame();
    EXPECT_TRUE(r.ui.element("h").hovered());
    EXPECT_TRUE(r.ui.element("f").focused());
    EXPECT_TRUE(r.ui.element("d").dragging());
    EXPECT_FALSE(r.ui.element("other").hovered());
    r.ui.end_frame();
}

// --- Binding -------------------------------------------------------------------------------------

// THE check brief 12 M0c calls for: the accessor must be handle-GENERIC, not CVar-shaped. This
// binds a handle type the engine has never heard of — the shape brief 15's store will have.
TEST(UiFacadeTest, BindIsHandleGenericNotCVarShaped)
{
    static_assert(accessor_handle<FakeStoreHandle>);

    FakeStoreHandle h{ 2.0f };
    binding<float> b = bind(h);
    ASSERT_TRUE(b.valid());
    EXPECT_FLOAT_EQ(b.get(), 2.0f);
    b.set(7.5f);
    EXPECT_FLOAT_EQ(h.v, 7.5f);
    EXPECT_FLOAT_EQ(b.get(), 7.5f);
}

// The immediate-only escape hatch: raw obj+member. Legal because it never outlives the build.
TEST(UiFacadeTest, BindRawMemberReadsAndWrites)
{
    Sun sun{ 1.0f };
    binding<float> b = bind(sun, &Sun::intensity);
    ASSERT_TRUE(b.valid());
    EXPECT_FLOAT_EQ(b.get(), 1.0f);
    b.set(3.0f);
    EXPECT_FLOAT_EQ(sun.intensity, 3.0f);
}

// --- Theme ---------------------------------------------------------------------------------------

TEST(UiFacadeTest, ElementsInheritThemeAndOverridePerElement)
{
    Rig r;
    r.theme.panel = color{ 1, 2, 3, 4 };
    r.theme.radius = 7;
    r.ui.begin_frame();
    r.ui.element("themed").themed().content([](Ui& u) {
        u.element("overridden").themed().color(color{ 9, 9, 9, 9 }).content([](Ui&) {});
    });
    r.builder.end(dimension{ 200, 200 });
    r.ui.end_frame();

    const layout_node* themed = r.builder.find(make_id("themed").hash);
    ASSERT_NE(themed, nullptr);
    EXPECT_EQ(themed->element.color.r, 1);        // inherited from the theme
    EXPECT_EQ(themed->element.radius, 7);

    const layout_node* over = r.builder.find(make_id("overridden").hash);
    ASSERT_NE(over, nullptr);
    EXPECT_EQ(over->element.color.r, 9);          // per-element override wins
    EXPECT_EQ(over->element.radius, 7);           // ...but only for what was overridden
}

// --- Sizing --------------------------------------------------------------------------------------

TEST(UiFacadeTest, SizingVocabularyResolves)
{
    Rig r;
    r.ui.begin_frame();
    // The HOST owns the root (see the note on Ui: the outermost end() carries the available size
    // and the text measurer, both of which are host concerns). Authoring inside it is what the real
    // pass does, so tests model it the same way.
    r.builder.begin(format{ .direction = direction::HORIZONTAL });
    r.ui.element("fixed_child").fixed(50, 20);
    r.ui.element("grow_child").grow();
    r.builder.end(dimension{ 200, 20 });
    r.ui.end_frame();

    const layout_node* fx = r.builder.find(make_id("fixed_child").hash);
    const layout_node* gw = r.builder.find(make_id("grow_child").hash);
    ASSERT_NE(fx, nullptr);
    ASSERT_NE(gw, nullptr);
    EXPECT_EQ(fx->box.dimension.width, 50);
    EXPECT_EQ(gw->box.dimension.width, 150);   // takes the remainder
}

// --- Parity --------------------------------------------------------------------------------------

// The M0c acceptance shape in miniature: author the SAME tree twice — once against the raw
// layout_builder the way brief 05 does it, once through the facade — and require the layout dumps
// to match. This is the mechanism that will police re-expressing the real screens; proving it on a
// faithful copy of `author_status_panel` keeps the claim honest rather than hypothetical.
TEST(UiFacadeTest, FacadeReproducesRawBuilderTreeExactly)
{
    const Theme theme{};
    const char* lines[] = { "String UI sandbox", "frame 120", "screen: all", "anchors: 24" };

    // Both sides are authored INSIDE a host-opened root and closed with end(available), exactly as
    // UIPass drives it. Getting this wrong is what made the first draft of this test fail: the
    // facade's content() closes with a plain end(), so if the panel IS the outermost container it
    // never receives the available size and every grow/fit resolves against nothing.

    // --- raw, as brief 05 authors it ---
    layout_builder raw;
    {
        raw.begin(format{ .direction = direction::VERTICAL });
        element panel{};
        panel.id = make_id("status");
        panel.color = theme.panel;
        panel.stroke_color = theme.stroke;
        panel.stroke_width = 2;
        panel.radius = 12;
        panel.shape = shape::ROUNDED_RECTANGLE;
        panel.sizing = size_fit();
        raw.begin(panel, format{ .padding = { 12, 12, 12, 12 }, .gap = 4,
                                 .direction = direction::VERTICAL });
        for (std::size_t i = 0; i < 4; ++i)
        {
            element row{};
            row.id = make_id("status_row");
            row.sizing = size_fit();
            row.color = (i == 0) ? theme.accent : theme.text;
            raw.add_text(row, lines[i], (i == 0) ? 26 : 20);
        }
        raw.end();                          // close the panel
        raw.end(dimension{ 800, 800 });     // close the root, laying out against the screen
    }

    // --- the same tree, through the facade ---
    layout_builder fluent;
    interaction state{};
    Motion motion;
    PanelStore panels;
    Ui ui{ fluent, state, motion, theme, panels };
    ui.begin_frame();
    fluent.begin(format{ .direction = direction::VERTICAL });
    ui.element("status").themed().radius(12).pad(12).gap(4).column().content([&](Ui& u) {
        for (std::size_t i = 0; i < 4; ++i)
            u.element("status_row").text(lines[i])
             .color(i == 0 ? theme.accent : theme.text)
             .font(i == 0 ? 26 : 20);
    });
    fluent.end(dimension{ 800, 800 });
    ui.end_frame();

    EXPECT_EQ(dump_layout(raw), dump_layout(fluent));
}

// --- Focusable is declared, not inferred from having an id ---------------------------------------

namespace
{
struct FocusRig
{
    layout_builder builder;
    interaction state{};
    Motion motion;
    Theme theme{};
    PanelStore panels;
    Ui ui{ builder, state, motion, theme, panels };
};

bool is_focusable(const layout_builder& b, std::string_view name)
{
    const layout_node* n = b.find(make_id(name).hash);
    return n != nullptr && n->element.focusable;
}
}  // namespace

TEST(UiFacadeTest, ButtonsAndClickHandlersDeclareFocusabilityWithoutBeingAskedTwice)
{
    FocusRig rig;
    rig.state.screen = dimension{ 400, 300 };
    rig.ui.begin_frame();
    rig.builder.begin(format{ .direction = direction::VERTICAL });
    rig.ui.button("btn").content([](Ui& u) { u.text("go"); });
    // Registering an activation handler IS the declaration — an element that can be clicked is
    // exactly what the gamepad's south button should reach, so saying it twice would be noise.
    rig.ui.element("handler").on_click([] {});
    // ...but a plain addressable element is NOT a focus target. This is the drag-handle case.
    rig.ui.element("chrome").fixed(10, 10);
    rig.builder.end(dimension{ 400, 300 });
    rig.ui.end_frame();

    EXPECT_TRUE(is_focusable(rig.builder, "btn"));
    EXPECT_TRUE(is_focusable(rig.builder, "handler"));
    EXPECT_FALSE(is_focusable(rig.builder, "chrome"));
}

// The case that started this: a panel's own chrome is addressable (it hovers, it drags, it
// captures the pointer) and must never be somewhere focus can land.
TEST(UiFacadeTest, PanelChromeIsAddressableButNotFocusable)
{
    FocusRig rig;
    rig.state.screen = dimension{ 400, 300 };
    rig.ui.begin_frame();
    rig.builder.begin(format{ .direction = direction::VERTICAL });
    rig.ui.panel("stats").title("Stats").initial({ 20, 20, 200, 150 }).content([](Ui& u) {
        u.button("inner").content([](Ui& u2) { u2.text("ok"); });
    });
    rig.builder.end(dimension{ 400, 300 });
    rig.ui.end_frame();

    // Addressable: all three exist in the tree and carry ids.
    ASSERT_NE(rig.builder.find(make_id("stats.title").hash), nullptr);
    ASSERT_NE(rig.builder.find(make_id("stats.grip").hash), nullptr);

    EXPECT_FALSE(is_focusable(rig.builder, "stats"));
    EXPECT_FALSE(is_focusable(rig.builder, "stats.title"));
    EXPECT_FALSE(is_focusable(rig.builder, "stats.grip"));
    // The control INSIDE the panel is what nav should find.
    EXPECT_TRUE(is_focusable(rig.builder, "inner"));
}
