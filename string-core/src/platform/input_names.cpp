#include <array>

#include <string/platform/input.hpp>

// Human-readable names for input codes. Exists for the rebinding UI: a binding list has to show what
// a key IS, and an enum value is not that.
//
// A switch rather than a table, because `KeyCode` is deliberately sparse (it follows the GLFW/SDL
// numbering, so it runs to 347 with large gaps). A 348-entry array to name ~100 keys would be mostly
// holes, and every gap would be a silent empty string rather than a visible fallback.
namespace string
{

std::string_view key_name(KeyCode k) noexcept
{
    switch (k)
    {
        case KeyCode::UNKNOWN:       return "—";
        case KeyCode::SPACE:         return "Space";
        case KeyCode::APOSTROPHE:    return "'";
        case KeyCode::COMMA:         return ",";
        case KeyCode::MINUS:         return "-";
        case KeyCode::PERIOD:        return ".";
        case KeyCode::SLASH:         return "/";
        case KeyCode::KEY_0:         return "0";
        case KeyCode::KEY_1:         return "1";
        case KeyCode::KEY_2:         return "2";
        case KeyCode::KEY_3:         return "3";
        case KeyCode::KEY_4:         return "4";
        case KeyCode::KEY_5:         return "5";
        case KeyCode::KEY_6:         return "6";
        case KeyCode::KEY_7:         return "7";
        case KeyCode::KEY_8:         return "8";
        case KeyCode::KEY_9:         return "9";
        case KeyCode::SEMICOLON:     return ";";
        case KeyCode::EQUAL:         return "=";
        case KeyCode::A:             return "A";
        case KeyCode::B:             return "B";
        case KeyCode::C:             return "C";
        case KeyCode::D:             return "D";
        case KeyCode::E:             return "E";
        case KeyCode::F:             return "F";
        case KeyCode::G:             return "G";
        case KeyCode::H:             return "H";
        case KeyCode::I:             return "I";
        case KeyCode::J:             return "J";
        case KeyCode::K:             return "K";
        case KeyCode::L:             return "L";
        case KeyCode::M:             return "M";
        case KeyCode::N:             return "N";
        case KeyCode::O:             return "O";
        case KeyCode::P:             return "P";
        case KeyCode::Q:             return "Q";
        case KeyCode::R:             return "R";
        case KeyCode::S:             return "S";
        case KeyCode::T:             return "T";
        case KeyCode::U:             return "U";
        case KeyCode::V:             return "V";
        case KeyCode::W:             return "W";
        case KeyCode::X:             return "X";
        case KeyCode::Y:             return "Y";
        case KeyCode::Z:             return "Z";
        case KeyCode::LEFT_BRACKET:  return "[";
        case KeyCode::BACKSLASH:     return "\\";
        case KeyCode::RIGHT_BRACKET: return "]";
        case KeyCode::GRAVE_ACCENT:  return "`";
        case KeyCode::ESCAPE:        return "Esc";
        case KeyCode::ENTER:         return "Enter";
        case KeyCode::TAB:           return "Tab";
        case KeyCode::BACKSPACE:     return "Backspace";
        case KeyCode::INSERT:        return "Insert";
        case KeyCode::DELETE:        return "Delete";
        case KeyCode::RIGHT:         return "Right";
        case KeyCode::LEFT:          return "Left";
        case KeyCode::DOWN:          return "Down";
        case KeyCode::UP:            return "Up";
        case KeyCode::PAGE_UP:       return "PgUp";
        case KeyCode::PAGE_DOWN:     return "PgDn";
        case KeyCode::HOME:          return "Home";
        case KeyCode::END:           return "End";
        case KeyCode::CAPS_LOCK:     return "CapsLock";
        case KeyCode::SCROLL_LOCK:   return "ScrollLock";
        case KeyCode::NUM_LOCK:      return "NumLock";
        case KeyCode::PRINT_SCREEN:  return "PrintScreen";
        case KeyCode::PAUSE:         return "Pause";
        case KeyCode::F1:            return "F1";
        case KeyCode::F2:            return "F2";
        case KeyCode::F3:            return "F3";
        case KeyCode::F4:            return "F4";
        case KeyCode::F5:            return "F5";
        case KeyCode::F6:            return "F6";
        case KeyCode::F7:            return "F7";
        case KeyCode::F8:            return "F8";
        case KeyCode::F9:            return "F9";
        case KeyCode::F10:           return "F10";
        case KeyCode::F11:           return "F11";
        case KeyCode::F12:           return "F12";
        case KeyCode::LEFT_SHIFT:    return "LShift";
        case KeyCode::LEFT_CONTROL:  return "LCtrl";
        case KeyCode::LEFT_ALT:      return "LAlt";
        case KeyCode::LEFT_SUPER:    return "LSuper";
        case KeyCode::RIGHT_SHIFT:   return "RShift";
        case KeyCode::RIGHT_CONTROL: return "RCtrl";
        case KeyCode::RIGHT_ALT:     return "RAlt";
        case KeyCode::RIGHT_SUPER:   return "RSuper";
    }
    // A code the enum knows but this switch does not. Visible as a gap rather than as "unbound",
    // which is the failure an empty string would produce.
    return "key?";
}

std::string_view button_name(MouseButton b) noexcept
{
    switch (b)
    {
        case MouseButton::LEFT:     return "Mouse1";
        case MouseButton::RIGHT:    return "Mouse2";
        case MouseButton::MIDDLE:   return "Mouse3";
        case MouseButton::BUTTON_4: return "Mouse4";
        case MouseButton::BUTTON_5: return "Mouse5";
        case MouseButton::BUTTON_6: return "Mouse6";
        case MouseButton::BUTTON_7: return "Mouse7";
        case MouseButton::BUTTON_8: return "Mouse8";
    }
    return "mouse?";
}

std::string_view button_name(GamepadButton b) noexcept
{
    switch (b)
    {
        case GamepadButton::A:              return "Pad A";
        case GamepadButton::B:              return "Pad B";
        case GamepadButton::X:              return "Pad X";
        case GamepadButton::Y:              return "Pad Y";
        case GamepadButton::BACK:           return "Pad Back";
        case GamepadButton::GUIDE:          return "Pad Guide";
        case GamepadButton::START:          return "Pad Start";
        case GamepadButton::LEFT_STICK:     return "Pad LS";
        case GamepadButton::RIGHT_STICK:    return "Pad RS";
        case GamepadButton::LEFT_SHOULDER:  return "Pad LB";
        case GamepadButton::RIGHT_SHOULDER: return "Pad RB";
        case GamepadButton::UP:             return "Pad Up";
        case GamepadButton::DOWN:           return "Pad Down";
        case GamepadButton::LEFT:           return "Pad Left";
        case GamepadButton::RIGHT:          return "Pad Right";
        case GamepadButton::COUNT:          break;
    }
    return "pad?";
}

}  // namespace string
