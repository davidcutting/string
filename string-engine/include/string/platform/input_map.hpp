#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <string/platform/input.hpp>

namespace String
{

// A remappable action layer over the polled Input. Gameplay binds semantic actions ("jump",
// "move_forward") to keys/buttons instead of reading raw scancodes, so controls can be rebound
// by editing the map. Digital actions answer held/pressed/released; axis actions compose a
// signed value from a positive/negative key pair (e.g. D/A -> +1/-1). Continuous look/aim still
// comes from Input::mouse_delta() (exposed here for convenience); routing gameplay through
// per-frame polled state, not an event queue, keeps it frame-coherent.
//
// Stateless beyond its bindings: every query reads the live Input, whose per-frame edge state is
// refreshed by the window each frame — so no InputMap update() is needed.
class InputMap
{
public:
    explicit InputMap(const Input& input) : input_(&input) {}

    // --- Binding setup (call once at startup; safe to rebind later) ---
    void bind_button(std::string action, KeyCode key);
    void bind_button(std::string action, MouseButton button);
    // Adds a +key/-key pair to an axis action (multiple pairs on one action sum together).
    void bind_axis(std::string action, KeyCode positive, KeyCode negative);
    void clear(std::string_view action);

    // --- Queries ---
    bool held(std::string_view action) const;       // any bound key/button currently down
    bool pressed(std::string_view action) const;    // any bound key/button went down this frame
    bool released(std::string_view action) const;   // any bound key/button went up this frame
    float axis(std::string_view action) const;       // sum of bound pairs, clamped to [-1, 1]

    glm::vec2 mouse_delta() const { return input_->mouse_delta(); }
    const Input& input() const { return *input_; }

private:
    // Transparent hashing so string_view queries don't allocate a std::string to look up.
    struct StringHash
    {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
    };
    template <typename V>
    using ActionMap = std::unordered_map<std::string, V, StringHash, std::equal_to<>>;

    struct ButtonBinding
    {
        std::vector<KeyCode> keys;
        std::vector<MouseButton> buttons;
    };
    using AxisPair = std::pair<KeyCode, KeyCode>;   // {positive, negative}

    const Input* input_;
    ActionMap<ButtonBinding> buttons_;
    ActionMap<std::vector<AxisPair>> axes_;
};

}  // namespace String
