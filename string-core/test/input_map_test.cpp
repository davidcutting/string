#include <gtest/gtest.h>

#include <algorithm>
#include <span>

#include <string/platform/input_map.hpp>

using namespace string;
using namespace string::literals;

namespace
{
constexpr ActionId kForward = "move_forward"_action;
constexpr ActionId kJump    = "jump"_action;
constexpr ActionId kCancel  = "ui.cancel"_action;

constexpr ActionId kChat    = "ctx.chat"_action;
constexpr ActionId kModal   = "ctx.modal"_action;

// A map with the three actions above bound, plus the raw Input backing it. Held together because
// InputMap holds a reference — a rig keeps the two lifetimes in step.
struct Rig
{
    Input input;
    InputMap map{ input };

    Rig()
    {
        map.bind_button("jump", KeyCode::SPACE);
        map.bind_button("ui.cancel", KeyCode::ESCAPE);
        map.bind_axis("move_forward", KeyCode::W, KeyCode::S);
    }

    // Press a key as an EDGE: new_frame() snapshots the previous state, so pressed() is true for
    // exactly one frame, the way the window backend drives it.
    void press(KeyCode k)
    {
        input.new_frame();
        input.set_key(k, true);
    }
};
}  // namespace

// --- Interning ------------------------------------------------------------------------------

TEST(ActionId, HashesAtCompileTimeAndDistinguishesNames)
{
    static_assert("jump"_action == action_id("jump"));
    static_assert(!("jump"_action == "duck"_action));
    static_assert("jump"_action.valid());
    // A default-constructed id is the "no action" sentinel, so a forgotten initialiser cannot
    // silently alias a real action.
    static_assert(!ActionId{}.valid());
    EXPECT_FALSE(action_id("").valid());
}

TEST(InputMap, BindingByNameRemembersItForRebindingUi)
{
    Rig r;
    EXPECT_EQ(r.map.name_of(kJump), "jump");
    // Binding by id alone is legal but leaves the action nameless — ids are for querying, names are
    // for humans.
    r.map.bind_button("nameless"_action, KeyCode::Q);
    EXPECT_TRUE(r.map.name_of("nameless"_action).empty());
}

TEST(InputMap, QueriesResolveByIdNotByString)
{
    Rig r;
    r.press(KeyCode::SPACE);
    EXPECT_TRUE(r.map.pressed(kJump));
    EXPECT_TRUE(r.map.held(kJump));
    EXPECT_FALSE(r.map.pressed("jump_but_different"_action));

    r.input.new_frame();          // the edge expires; the key is still down
    EXPECT_FALSE(r.map.pressed(kJump));
    EXPECT_TRUE(r.map.held(kJump));

    r.input.new_frame();
    r.input.set_key(KeyCode::SPACE, false);
    EXPECT_TRUE(r.map.released(kJump));
}

TEST(InputMap, AxisSumsItsKeyPairAndClamps)
{
    Rig r;
    r.input.set_key(KeyCode::W, true);
    EXPECT_FLOAT_EQ(r.map.axis(kForward), 1.0f);
    r.input.set_key(KeyCode::S, true);
    EXPECT_FLOAT_EQ(r.map.axis(kForward), 0.0f);   // both down cancel
    r.input.set_key(KeyCode::W, false);
    EXPECT_FLOAT_EQ(r.map.axis(kForward), -1.0f);
}

// --- The context stack ----------------------------------------------------------------------

TEST(InputMap, BaseContextIsAlwaysPresentAndCannotBePopped)
{
    Rig r;
    EXPECT_TRUE(r.map.context_active(InputMap::base_context()));
    r.map.pop_context(InputMap::base_context());
    EXPECT_TRUE(r.map.context_active(InputMap::base_context()));
    EXPECT_EQ(r.map.context_depth(), 1u);
}

// The case the whole mechanism exists for: chat takes the keys it needs and leaves the rest alone,
// which a single suppress-everything boolean cannot express.
TEST(InputMap, SelectiveContextClaimsOnlyItsOwnActionsAndLetsTheRestThrough)
{
    Rig r;
    const ActionId claims[] = { kCancel };
    r.map.push_context(kChat, claims);

    r.press(KeyCode::ESCAPE);
    EXPECT_FALSE(r.map.pressed(kCancel)) << "chat claimed cancel; gameplay must not see it";
    EXPECT_TRUE(r.map.pressed(kCancel, kChat)) << "...but chat itself receives it";

    // Movement was never claimed, so it still reaches gameplay while chat is open.
    r.input.set_key(KeyCode::W, true);
    EXPECT_FLOAT_EQ(r.map.axis(kForward), 1.0f);
}

TEST(InputMap, ExclusiveContextClaimsEverythingBelowIt)
{
    Rig r;
    r.map.push_context(kModal);

    r.input.set_key(KeyCode::W, true);
    EXPECT_FLOAT_EQ(r.map.axis(kForward), 0.0f);
    EXPECT_TRUE(r.map.blocked(kForward));
    EXPECT_FLOAT_EQ(r.map.axis(kForward, kModal), 1.0f);   // the modal itself still reads

    r.map.pop_context(kModal);
    EXPECT_FLOAT_EQ(r.map.axis(kForward), 1.0f);
}

// Priority is stack ORDER, so a modal pushed over chat takes what chat had claimed.
TEST(InputMap, HigherContextsOutrankLowerOnes)
{
    Rig r;
    const ActionId claims[] = { kCancel };
    r.map.push_context(kChat, claims);
    r.map.push_context(kModal);

    r.press(KeyCode::ESCAPE);
    EXPECT_TRUE(r.map.pressed(kCancel, kModal));
    EXPECT_FALSE(r.map.pressed(kCancel, kChat)) << "the modal above chat takes it";
    EXPECT_FALSE(r.map.pressed(kCancel));
}

// Surfaces close out of order, so popping must be by identity rather than "the top one".
TEST(InputMap, PoppingRemovesByIdentityNotPosition)
{
    Rig r;
    const ActionId claims[] = { kCancel };
    r.map.push_context(kChat, claims);
    r.map.push_context(kModal);

    r.map.pop_context(kChat);      // the one UNDERNEATH
    EXPECT_FALSE(r.map.context_active(kChat));
    EXPECT_TRUE(r.map.context_active(kModal));
    EXPECT_EQ(r.map.context_depth(), 2u);   // base + modal

    r.map.pop_context(kChat);      // a close path that runs twice must not be a special case
    EXPECT_EQ(r.map.context_depth(), 2u);
}

TEST(InputMap, PushingAnActiveContextRaisesItRatherThanDuplicating)
{
    Rig r;
    const ActionId claims[] = { kCancel };
    r.map.push_context(kChat, claims);
    r.map.push_context(kModal);
    r.map.push_context(kChat, claims);   // re-focus chat

    EXPECT_EQ(r.map.context_depth(), 3u) << "base + modal + chat, not a second chat";
    r.press(KeyCode::ESCAPE);
    EXPECT_TRUE(r.map.pressed(kCancel, kChat)) << "chat is now above the modal";
}

// An inactive reader gets nothing, rather than silently falling back to base. Falling back would
// hand a closed surface live gameplay input, which is far worse than obviously receiving nothing.
TEST(InputMap, ReadingAsAnInactiveContextYieldsNothing)
{
    Rig r;
    r.press(KeyCode::SPACE);
    EXPECT_TRUE(r.map.pressed(kJump));
    EXPECT_FALSE(r.map.pressed(kJump, kChat));
}

TEST(InputMap, ScopedContextPopsOnScopeExit)
{
    Rig r;
    {
        InputMap::ScopedContext modal(r.map, kModal);
        EXPECT_TRUE(r.map.context_active(kModal));
        r.input.set_key(KeyCode::W, true);
        EXPECT_FLOAT_EQ(r.map.axis(kForward), 0.0f);
    }
    EXPECT_FALSE(r.map.context_active(kModal));
    EXPECT_FLOAT_EQ(r.map.axis(kForward), 1.0f);
}

// --- text_capture, now expressed IN the stack rather than beside it -------------------------
//
// Behaviour must be exactly what it was: the flag suppresses every action routed through the map.
// What changed is that it is now the top-priority exclusive context rather than an early-out, which
// is what gives a text surface a seam to read through if it ever wants one.

TEST(InputMap, TextCaptureSuppressesEveryActionJustAsItAlwaysDid)
{
    Rig r;
    r.press(KeyCode::SPACE);
    ASSERT_TRUE(r.map.pressed(kJump));

    r.input.set_text_capture(true);
    EXPECT_FALSE(r.map.pressed(kJump));
    EXPECT_FALSE(r.map.held(kJump));
    r.input.set_key(KeyCode::W, true);
    EXPECT_FLOAT_EQ(r.map.axis(kForward), 0.0f);

    r.input.set_text_capture(false);
    EXPECT_TRUE(r.map.held(kJump));
}

// M2 CORRECTION. The first draft placed the text context above the WHOLE stack, so a capturing
// surface blocked everything including the UI's own actions — which meant the console that raised
// the flag could never receive the Escape that closes it, and the raw-key workaround it already
// carried for its toggle would have had to stay.
//
// Text capture's contract has always been that typing must not drive the camera: it suppresses
// GAMEPLAY, which reads at base. So it sits directly ABOVE BASE, and anything the app deliberately
// pushed higher is unaffected.
TEST(InputMap, TextCaptureSuppressesBaseButNotContextsPushedAboveIt)
{
    Rig r;
    r.map.push_context(kModal);
    r.input.set_text_capture(true);
    r.press(KeyCode::SPACE);

    EXPECT_TRUE(r.map.context_active(InputMap::text_context()));
    EXPECT_FALSE(r.map.pressed(kJump)) << "gameplay (base) is suppressed while typing";
    EXPECT_TRUE(r.map.pressed(kJump, kModal))
        << "a context pushed above base asked to be there and still reads";
}

// REGRESSION (2026-08-03). The UI pushes a context purely to have a POSITION on the stack to read
// from — it claims nothing by itself; what gets claimed is a per-surface decision. Reaching for the
// one-argument push (the EXCLUSIVE overload) put a blanket claim over base and silently killed all
// gameplay input: camera and every debug key, while the UI itself kept working, so nothing looked
// broken from the UI side and no capture gate could see it.
//
// The distinction between the two overloads is therefore load-bearing, and this pins it.
TEST(InputMap, AContextWithNoClaimsTakesNothingFromBase)
{
    Rig r;
    r.map.push_context(kChat, std::span<const ActionId>{});

    r.press(KeyCode::SPACE);
    EXPECT_TRUE(r.map.pressed(kJump)) << "an empty claim must not suppress gameplay";
    EXPECT_TRUE(r.map.pressed(kJump, kChat)) << "...and the pusher can still read";
    EXPECT_FALSE(r.map.blocked(kJump));

    // The one-argument overload is the opposite, deliberately.
    r.map.push_context(kModal);
    EXPECT_TRUE(r.map.blocked(kJump));
}

// --- M3: priority, so "unswallowable" is a property of the context ---------------------------
//
// A plain stack cannot express this. Whatever is pushed LAST sits highest, so a dialog opened after
// startup would take a screenshot key away from the player — and the surface doing it would look
// entirely correct from its own side. Priority makes the guarantee independent of timing.

namespace { constexpr ActionId kShot = "system.screenshot"_action; }

TEST(InputMap, SystemPriorityOutranksASurfacePushedLater)
{
    Rig r;
    r.map.bind_button("system.screenshot", KeyCode::F12);

    const ActionId claims[] = { kShot };
    r.map.push_context("ctx.system"_action, claims, InputMap::kSystemPriority);

    // A modal opened AFTERWARDS, claiming everything — the case that breaks a naive stack.
    r.map.push_context(kModal);

    r.press(KeyCode::F12);
    EXPECT_TRUE(r.map.pressed(kShot, "ctx.system"_action))
        << "a system claim must survive a later, broader surface";
    EXPECT_FALSE(r.map.pressed(kShot)) << "...while still being taken off base";
}

// Within a tier, push order still decides — a newly opened panel outranks an older one.
TEST(InputMap, EqualPriorityStillOrdersByPushOrder)
{
    Rig r;
    const ActionId claims[] = { kCancel };
    r.map.push_context(kChat, claims);
    r.map.push_context(kModal, claims);   // same tier, pushed later

    r.press(KeyCode::ESCAPE);
    EXPECT_TRUE(r.map.pressed(kCancel, kModal));
    EXPECT_FALSE(r.map.pressed(kCancel, kChat));
}

// Priority holds regardless of the order the two are pushed in, which is the whole guarantee.
TEST(InputMap, PriorityIsIndependentOfPushOrder)
{
    const ActionId claims[] = { kShot };
    for (int order = 0; order < 2; ++order)
    {
        Rig r;
        r.map.bind_button("system.screenshot", KeyCode::F12);
        if (order == 0)
        {
            r.map.push_context("ctx.system"_action, claims, InputMap::kSystemPriority);
            r.map.push_context(kModal);
        }
        else
        {
            r.map.push_context(kModal);
            r.map.push_context("ctx.system"_action, claims, InputMap::kSystemPriority);
        }
        r.press(KeyCode::F12);
        EXPECT_TRUE(r.map.pressed(kShot, "ctx.system"_action)) << "push order " << order;
    }
}

// --- M3: enumeration, for the rebinding UI ----------------------------------------------------

TEST(InputMap, EnumerationIsStablyOrderedAndCarriesNamesAndBindings)
{
    Rig r;
    r.map.bind_button("zzz_last", KeyCode::Z);
    r.map.bind_button("aaa_first", KeyCode::A);

    const std::vector<InputMap::action_info> a = r.map.actions();
    ASSERT_GE(a.size(), 5u);

    // SORTED BY NAME. An unordered_map iterates in bucket order, so a list built from it would
    // reshuffle between frames and be impossible to click.
    for (std::size_t i = 1; i < a.size(); ++i)
        if (!a[i - 1].name.empty() && !a[i].name.empty())
            EXPECT_LE(a[i - 1].name, a[i].name);

    const auto jump = std::find_if(a.begin(), a.end(),
                                   [](const auto& x) { return x.name == "jump"; });
    ASSERT_NE(jump, a.end());
    ASSERT_EQ(jump->keys.size(), 1u);
    EXPECT_EQ(jump->keys[0], KeyCode::SPACE);
    EXPECT_FALSE(jump->axis);

    // Axis actions are reported as axes, so a rebinder can refuse to rebind them from a single key.
    const auto fwd = std::find_if(a.begin(), a.end(),
                                  [](const auto& x) { return x.name == "move_forward"; });
    ASSERT_NE(fwd, a.end());
    EXPECT_TRUE(fwd->axis);
}

TEST(InputMap, RebindingReplacesTheOldKey)
{
    Rig r;
    r.map.clear(kJump);
    r.map.bind_button("jump", KeyCode::J);

    r.press(KeyCode::J);
    EXPECT_TRUE(r.map.pressed(kJump));
    r.input.new_frame();
    r.input.set_key(KeyCode::J, false);

    r.press(KeyCode::SPACE);
    EXPECT_FALSE(r.map.pressed(kJump)) << "the old binding must be gone, not merely shadowed";
}

TEST(KeyName, NamesEveryBoundCodeAndFallsBackVisibly)
{
    EXPECT_EQ(key_name(KeyCode::SPACE), "Space");
    EXPECT_EQ(key_name(KeyCode::F12), "F12");
    EXPECT_EQ(key_name(KeyCode::GRAVE_ACCENT), "`");
    EXPECT_EQ(button_name(MouseButton::LEFT), "Mouse1");
    EXPECT_EQ(button_name(GamepadButton::A), "Pad A");
    // An unmapped code reads as a gap, never as an empty cell that looks like "unbound".
    EXPECT_FALSE(key_name(static_cast<KeyCode>(999)).empty());
}

TEST(Input, FirstKeyPressedReportsThisFramesEdgeOnly)
{
    Input in;
    EXPECT_EQ(in.first_key_pressed(), KeyCode::UNKNOWN);

    in.new_frame();
    in.set_key(KeyCode::K, true);
    EXPECT_EQ(in.first_key_pressed(), KeyCode::K);

    in.new_frame();                      // still held, but the EDGE has passed
    EXPECT_EQ(in.first_key_pressed(), KeyCode::UNKNOWN);
}
