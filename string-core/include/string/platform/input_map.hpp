#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <string/platform/action_id.hpp>
#include <string/platform/input.hpp>

namespace String
{

// A remappable action layer over the polled Input, with a CONTEXT STACK deciding who receives what.
//
// Gameplay binds semantic actions ("jump", "move_forward") to keys/buttons instead of raw scancodes,
// so controls can be rebound. Digital actions answer held/pressed/released; axis actions compose a
// signed value from a positive/negative key pair (e.g. D/A -> +1/-1). Continuous look/aim still comes
// from Input::mouse_delta().
//
// POLLED, NOT QUEUED, and that is the latency argument: gameplay asks "is strafe down" once per frame
// and gets an integer lookup plus one bit, with no allocation, no event dispatch, and no dependency
// on the UI's layout tree. Nothing in this class walks a tree.
//
// --- THE CONTEXT STACK ---------------------------------------------------------------------------
//
// This generalises what `Input::text_capture` did as a single boolean. That flag meant "a modal text
// surface is open, so suppress EVERY action" — one claimant, one priority, all-or-nothing. The same
// shape is needed for chat swallowing WASD, a modal dialog taking Escape, and a hotbar taking 1-9,
// and stacking booleans does not compose.
//
// So: contexts are pushed and popped by the app, forming a stack. A context claims either EVERYTHING
// below it (exclusive — what text_capture does) or a specific list of actions. A query names the
// context it reads as, and an action reaches that reader unless some context ABOVE it claims that
// action.
//
// THE STACK IS IMMEDIATE APP STATE, deliberately, and NOT derived from the UI's element tree. UI
// focus is resolved post-layout, so anything read off the tree is a frame stale — and a frame-stale
// "is chat open?" means one frame of movement keys leaking into the character on the frame chat
// opens. Push/pop is the app saying so now. (Focus WITHIN a UI scope can stay tree-derived; that
// staleness is the same one hover and press already live with, and it never reaches gameplay.)
//
// Stateless beyond its bindings and its stack: every query reads the live Input, whose per-frame edge
// state is refreshed by the window each frame, so there is no update() to call.
class InputMap
{
public:
    explicit InputMap(const Input& input) : input_(&input)
    {
        stack_.push_back({ base_context(), kBasePriority, true, {} });
    }

    // The always-present bottom of the stack. Gameplay and debug keys read as this, which is why it
    // is the default for every query — anything pushed above them can take input away, and nothing
    // can take input away from a reader below.
    [[nodiscard]] static constexpr ActionId base_context() noexcept { return action_id("input.base"); }

    // The implicit context `Input::text_capture` raises. It sits DIRECTLY ABOVE BASE — its contract
    // is that typing must not drive the camera, i.e. it suppresses GAMEPLAY — so a context the app
    // deliberately pushes above base (the UI's own, a modal) still receives input while a text
    // surface is capturing. That is what lets Escape close the very console that raised the flag.
    [[nodiscard]] static constexpr ActionId text_context() noexcept { return action_id("input.text"); }

    // --- Context stack ---------------------------------------------------------------------------
    //
    // PRIORITY, not push order, decides who outranks whom. A plain stack cannot express "nothing may
    // ever swallow this": whatever is pushed last sits highest, so a dialog opened later would take a
    // screenshot key away from the player. Contexts are therefore kept sorted by priority, and push
    // order only breaks ties (later push wins among equals, which is what makes a newly opened
    // surface outrank an older one at the same tier).
    static constexpr int kBasePriority = 0;
    static constexpr int kSurfacePriority = 100;   // ordinary app surfaces: panels, dialogs, chat
    static constexpr int kSystemPriority = 1000;   // screenshot, push-to-talk: unswallowable

    // Claims EVERYTHING below it. For modal surfaces: a dialog, the console, a full-screen map.
    void push_context(ActionId id, int priority = kSurfacePriority);
    // Claims only `actions`; anything else falls through to the contexts below. For a chat bar that
    // wants the text keys and Escape but should not stop the camera, or a hotbar taking 1-9.
    void push_context(ActionId id, std::span<const ActionId> actions,
                      int priority = kSurfacePriority);
    // Removes `id` wherever it sits (not necessarily the top — surfaces close out of order). No-op if
    // absent, so a close path may run twice without special-casing.
    void pop_context(ActionId id);
    [[nodiscard]] bool context_active(ActionId id) const noexcept;
    [[nodiscard]] std::size_t context_depth() const noexcept { return stack_.size(); }

    // RAII push/pop, for a context whose life matches a scope.
    class ScopedContext
    {
    public:
        ScopedContext(InputMap& map, ActionId id, int priority = kSurfacePriority)
            : map_(&map), id_(id) { map.push_context(id, priority); }
        ScopedContext(InputMap& map, ActionId id, std::span<const ActionId> actions,
                      int priority = kSurfacePriority)
            : map_(&map), id_(id) { map.push_context(id, actions, priority); }
        ~ScopedContext() { if (map_ != nullptr) map_->pop_context(id_); }
        ScopedContext(const ScopedContext&) = delete;
        ScopedContext& operator=(const ScopedContext&) = delete;
        ScopedContext(ScopedContext&& o) noexcept : map_(o.map_), id_(o.id_) { o.map_ = nullptr; }
        ScopedContext& operator=(ScopedContext&&) = delete;
    private:
        InputMap* map_;
        ActionId id_;
    };

    // --- Binding setup (call once at startup; safe to rebind later) -------------------------------
    // The string overloads intern the name AND remember it, which is what the rebinding UI and debug
    // output read back. Binding by ActionId alone is legal but leaves the action nameless.
    void bind_button(ActionId action, KeyCode key);
    void bind_button(ActionId action, MouseButton button);
    void bind_button(ActionId action, GamepadButton button);
    void bind_button(std::string_view action, KeyCode key);
    void bind_button(std::string_view action, MouseButton button);
    void bind_button(std::string_view action, GamepadButton button);
    // Adds a +key/-key pair to an axis action (multiple pairs on one action sum together).
    void bind_axis(ActionId action, KeyCode positive, KeyCode negative);
    void bind_axis(std::string_view action, KeyCode positive, KeyCode negative);
    void clear(ActionId action);

    // The name an action was bound under, or empty. For the rebinding UI — never for lookup.
    [[nodiscard]] std::string_view name_of(ActionId action) const;

    // --- Enumeration, for a rebinding UI --------------------------------------------------------
    // Everything currently bound, in a stable order (sorted by name, so a list does not reshuffle
    // between frames as the hash map rehashes — an immediate-mode list that reorders itself is
    // unusable). Rebuilt per call: this is a UI-rate operation, not a per-frame one.
    struct action_info
    {
        ActionId id;
        std::string_view name;      // empty when bound by id alone
        std::vector<KeyCode> keys;
        std::vector<MouseButton> buttons;
        std::vector<GamepadButton> pad;
        bool axis = false;          // an axis action: its pairs are not shown as buttons
    };
    [[nodiscard]] std::vector<action_info> actions() const;

    // --- Queries ----------------------------------------------------------------------------------
    // `reader` is the context asking. The default is the base context, so gameplay and debug code
    // read exactly as they always have and anything pushed above them takes precedence.
    [[nodiscard]] bool held(ActionId action, ActionId reader = base_context()) const;
    [[nodiscard]] bool pressed(ActionId action, ActionId reader = base_context()) const;
    [[nodiscard]] bool released(ActionId action, ActionId reader = base_context()) const;
    [[nodiscard]] float axis(ActionId action, ActionId reader = base_context()) const;

    // Whether `action` is claimed by some context above `reader`. Exposed because a UI that greys out
    // a keybind, or a HUD that dims a suppressed hotbar slot, needs to SHOW suppression rather than
    // merely experience it.
    [[nodiscard]] bool blocked(ActionId action, ActionId reader = base_context()) const;

    [[nodiscard]] glm::vec2 mouse_delta() const { return input_->mouse_delta(); }
    [[nodiscard]] const Input& input() const { return *input_; }

private:
    struct ButtonBinding
    {
        std::vector<KeyCode> keys;
        std::vector<MouseButton> buttons;
        // Controller-first UI is a stated goal, so a pad button is a first-class binding target rather
        // than something the host special-cases on the way in.
        std::vector<GamepadButton> pad;
    };
    using AxisPair = std::pair<KeyCode, KeyCode>;   // {positive, negative}

    struct Context
    {
        ActionId id;
        int priority = kSurfacePriority;
        bool exclusive = false;
        std::vector<ActionId> claims;

        [[nodiscard]] bool claims_action(ActionId a) const noexcept
        {
            if (exclusive) return true;
            for (const ActionId c : claims)
                if (c == a) return true;
            return false;
        }
    };

    [[nodiscard]] const ButtonBinding* buttons_for(ActionId action, ActionId reader) const;

    const Input* input_;
    // Keyed by hash, so a query neither hashes nor compares a string. std::unordered_map is ample at
    // the tens-of-actions scale this runs at; if action counts ever reach the point where the node
    // chasing shows up, a flat sorted vector is the known next step.
    std::unordered_map<std::uint64_t, ButtonBinding> buttons_;
    std::unordered_map<std::uint64_t, std::vector<AxisPair>> axes_;
    std::unordered_map<std::uint64_t, std::string> names_;
    // Index 0 is the base context; the back is the top of the stack.
    std::vector<Context> stack_;
};

}  // namespace String
