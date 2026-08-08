#include <algorithm>

#include <string/platform/input_map.hpp>

namespace string
{

// --- Context stack ---------------------------------------------------------------------------

namespace
{
// Insert keeping the stack sorted by priority ASCENDING, after every entry of equal priority — so a
// newly pushed surface outranks an older one at the same tier, while a higher tier stays above both
// no matter when it arrived. That last part is the point: "nothing may swallow this" is a property
// of the context, not of when it happened to be pushed.
template <typename Vec, typename Ctx>
void insert_by_priority(Vec& stack, Ctx&& c)
{
    const auto at = std::upper_bound(stack.begin(), stack.end(), c.priority,
                                     [](int p, const auto& e) { return p < e.priority; });
    stack.insert(at, std::forward<Ctx>(c));
}
}  // namespace

void InputMap::push_context(ActionId id, int priority)
{
    if (!id.valid()) return;
    pop_context(id);   // re-pushing RAISES an existing context rather than duplicating it
    insert_by_priority(stack_, Context{ id, priority, true, {} });
}

void InputMap::push_context(ActionId id, std::span<const ActionId> actions, int priority)
{
    if (!id.valid()) return;
    pop_context(id);
    insert_by_priority(stack_, Context{ id, priority, false,
                                        std::vector<ActionId>(actions.begin(), actions.end()) });
}

void InputMap::pop_context(ActionId id)
{
    // The base context is structural, not a claim — removing it would leave every reader dangling.
    if (id == base_context()) return;
    const auto it = std::find_if(stack_.begin(), stack_.end(),
                                 [id](const Context& c) { return c.id == id; });
    if (it != stack_.end()) stack_.erase(it);
}

bool InputMap::context_active(ActionId id) const noexcept
{
    if (id == text_context()) return input_->text_capture();
    return std::any_of(stack_.begin(), stack_.end(),
                       [id](const Context& c) { return c.id == id; });
}

bool InputMap::blocked(ActionId action, ActionId reader) const
{
    // The text context sits DIRECTLY ABOVE BASE, not above the whole stack.
    //
    // That placement is its documented contract: text capture exists so typing does not drive the
    // camera — it suppresses GAMEPLAY, which reads at base. Putting it above everything (as the first
    // draft did) also blocked the UI's own actions, which meant a console that had raised text
    // capture could never receive the Escape that closes it. The console already worked around that
    // by reading its toggle key raw; this removes the need for the workaround rather than enshrining
    // it. Anything the app deliberately pushes ABOVE base asked to be there and is not suppressed.
    if (input_->text_capture() && reader == base_context()) return true;
    if (reader == text_context()) return false;   // it holds no claims of its own to answer

    const auto at = std::find_if(stack_.begin(), stack_.end(),
                                 [reader](const Context& c) { return c.id == reader; });
    // A reader whose context is not on the stack is not active, so it receives nothing. Silent
    // no-input is the correct answer AND an obvious symptom — far better than falling back to the
    // base context, which would hand a closed surface live gameplay input.
    if (at == stack_.end()) return true;

    for (auto above = at + 1; above != stack_.end(); ++above)
        if (above->claims_action(action)) return true;
    return false;
}

// --- Binding ---------------------------------------------------------------------------------

void InputMap::bind_button(ActionId action, KeyCode key)
{
    if (!action.valid()) return;
    buttons_[action.hash].keys.push_back(key);
}

void InputMap::bind_button(ActionId action, MouseButton button)
{
    if (!action.valid()) return;
    buttons_[action.hash].buttons.push_back(button);
}

void InputMap::bind_button(ActionId action, GamepadButton button)
{
    if (!action.valid()) return;
    buttons_[action.hash].pad.push_back(button);
}

void InputMap::bind_button(std::string_view action, GamepadButton button)
{
    const ActionId id = action_id(action);
    if (!id.valid()) return;
    names_.try_emplace(id.hash, action);
    bind_button(id, button);
}

void InputMap::bind_button(std::string_view action, KeyCode key)
{
    const ActionId id = action_id(action);
    if (!id.valid()) return;
    names_.try_emplace(id.hash, action);
    bind_button(id, key);
}

void InputMap::bind_button(std::string_view action, MouseButton button)
{
    const ActionId id = action_id(action);
    if (!id.valid()) return;
    names_.try_emplace(id.hash, action);
    bind_button(id, button);
}

void InputMap::bind_axis(ActionId action, KeyCode positive, KeyCode negative)
{
    if (!action.valid()) return;
    axes_[action.hash].push_back({ positive, negative });
}

void InputMap::bind_axis(std::string_view action, KeyCode positive, KeyCode negative)
{
    const ActionId id = action_id(action);
    if (!id.valid()) return;
    names_.try_emplace(id.hash, action);
    bind_axis(id, positive, negative);
}

void InputMap::clear(ActionId action)
{
    buttons_.erase(action.hash);
    axes_.erase(action.hash);
    names_.erase(action.hash);
}

std::string_view InputMap::name_of(ActionId action) const
{
    const auto it = names_.find(action.hash);
    return it == names_.end() ? std::string_view{} : std::string_view{ it->second };
}

// --- Enumeration -----------------------------------------------------------------------------

std::vector<InputMap::action_info> InputMap::actions() const
{
    std::vector<action_info> out;
    out.reserve(buttons_.size() + axes_.size());

    for (const auto& [hash, b] : buttons_)
        out.push_back({ ActionId{ hash }, name_of(ActionId{ hash }), b.keys, b.buttons, b.pad, false });
    for (const auto& [hash, pairs] : axes_)
    {
        (void)pairs;
        out.push_back({ ActionId{ hash }, name_of(ActionId{ hash }), {}, {}, {}, true });
    }

    // SORTED BY NAME, and it is not cosmetic: an unordered_map iterates in whatever order its
    // buckets happen to be in, so an immediate-mode list built from it would reshuffle itself
    // between frames and be impossible to click. Nameless actions sort last, by hash, so the order
    // is still total and still stable.
    std::sort(out.begin(), out.end(), [](const action_info& a, const action_info& b) {
        if (a.name.empty() != b.name.empty()) return b.name.empty();
        if (!a.name.empty()) return a.name < b.name;
        return a.id.hash < b.id.hash;
    });
    return out;
}

// --- Queries ---------------------------------------------------------------------------------

const InputMap::ButtonBinding* InputMap::buttons_for(ActionId action, ActionId reader) const
{
    if (blocked(action, reader)) return nullptr;
    const auto it = buttons_.find(action.hash);
    return it == buttons_.end() ? nullptr : &it->second;
}

bool InputMap::held(ActionId action, ActionId reader) const
{
    const ButtonBinding* b = buttons_for(action, reader);
    if (b == nullptr) return false;
    return std::any_of(b->keys.begin(), b->keys.end(),
                       [this](KeyCode k) { return input_->key_down(k); })
        || std::any_of(b->buttons.begin(), b->buttons.end(),
                       [this](MouseButton m) { return input_->mouse_button_down(m); })
        || std::any_of(b->pad.begin(), b->pad.end(),
                       [this](GamepadButton g) { return input_->gamepad_down(g); });
}

bool InputMap::pressed(ActionId action, ActionId reader) const
{
    const ButtonBinding* b = buttons_for(action, reader);
    if (b == nullptr) return false;
    return std::any_of(b->keys.begin(), b->keys.end(),
                       [this](KeyCode k) { return input_->key_pressed(k); })
        || std::any_of(b->buttons.begin(), b->buttons.end(),
                       [this](MouseButton m) { return input_->mouse_button_pressed(m); })
        || std::any_of(b->pad.begin(), b->pad.end(),
                       [this](GamepadButton g) { return input_->gamepad_pressed(g); });
}

bool InputMap::released(ActionId action, ActionId reader) const
{
    const ButtonBinding* b = buttons_for(action, reader);
    if (b == nullptr) return false;
    return std::any_of(b->keys.begin(), b->keys.end(),
                       [this](KeyCode k) { return input_->key_released(k); })
        || std::any_of(b->buttons.begin(), b->buttons.end(),
                       [this](MouseButton m) { return input_->mouse_button_released(m); })
        || std::any_of(b->pad.begin(), b->pad.end(),
                       [this](GamepadButton g) { return input_->gamepad_released(g); });
}

float InputMap::axis(ActionId action, ActionId reader) const
{
    if (blocked(action, reader)) return 0.0f;
    const auto it = axes_.find(action.hash);
    if (it == axes_.end()) return 0.0f;

    float value = 0.0f;
    for (const AxisPair& pair : it->second)
    {
        if (input_->key_down(pair.first))  value += 1.0f;
        if (input_->key_down(pair.second)) value -= 1.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

}  // namespace string
