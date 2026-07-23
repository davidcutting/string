#include <algorithm>

#include <string/platform/input_map.hpp>

namespace String
{

void InputMap::bind_button(std::string action, KeyCode key)
{
    buttons_[std::move(action)].keys.push_back(key);
}

void InputMap::bind_button(std::string action, MouseButton button)
{
    buttons_[std::move(action)].buttons.push_back(button);
}

void InputMap::bind_axis(std::string action, KeyCode positive, KeyCode negative)
{
    axes_[std::move(action)].push_back({ positive, negative });
}

void InputMap::clear(std::string_view action)
{
    if (auto it = buttons_.find(action); it != buttons_.end())
    {
        buttons_.erase(it);
    }
    if (auto it = axes_.find(action); it != axes_.end())
    {
        axes_.erase(it);
    }
}

bool InputMap::held(std::string_view action) const
{
    // Brief 06: a modal text surface (console) suppresses ALL gameplay actions routed through the
    // map, so typing/toggling in the console never drives the camera or debug keys.
    if (input_->text_capture()) return false;
    const auto it = buttons_.find(action);
    if (it == buttons_.end())
    {
        return false;
    }
    const ButtonBinding& binding = it->second;
    return std::any_of(binding.keys.begin(), binding.keys.end(),
                       [this](KeyCode k) { return input_->key_down(k); })
        || std::any_of(binding.buttons.begin(), binding.buttons.end(),
                       [this](MouseButton b) { return input_->mouse_button_down(b); });
}

bool InputMap::pressed(std::string_view action) const
{
    if (input_->text_capture()) return false;
    const auto it = buttons_.find(action);
    if (it == buttons_.end())
    {
        return false;
    }
    const ButtonBinding& binding = it->second;
    return std::any_of(binding.keys.begin(), binding.keys.end(),
                       [this](KeyCode k) { return input_->key_pressed(k); })
        || std::any_of(binding.buttons.begin(), binding.buttons.end(),
                       [this](MouseButton b) { return input_->mouse_button_pressed(b); });
}

bool InputMap::released(std::string_view action) const
{
    if (input_->text_capture()) return false;
    const auto it = buttons_.find(action);
    if (it == buttons_.end())
    {
        return false;
    }
    const ButtonBinding& binding = it->second;
    return std::any_of(binding.keys.begin(), binding.keys.end(),
                       [this](KeyCode k) { return input_->key_released(k); })
        || std::any_of(binding.buttons.begin(), binding.buttons.end(),
                       [this](MouseButton b) { return input_->mouse_button_released(b); });
}

float InputMap::axis(std::string_view action) const
{
    if (input_->text_capture()) return 0.0f;
    const auto it = axes_.find(action);
    if (it == axes_.end())
    {
        return 0.0f;
    }
    float value = 0.0f;
    for (const AxisPair& pair : it->second)
    {
        if (input_->key_down(pair.first))  value += 1.0f;
        if (input_->key_down(pair.second)) value -= 1.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

}  // namespace String
