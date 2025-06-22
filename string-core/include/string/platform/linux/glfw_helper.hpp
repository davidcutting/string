#pragma once

#include <string/platform/input.hpp>
#include <string/platform/event.hpp>
#include <GLFW/glfw3.h>

namespace String
{

constexpr KeyCode glfw_key_to_keycode(int glfw_key)
{
    // Handle GLFW_KEY_UNKNOWN -> KeyCode::UNKNOWN (0)
    if (glfw_key == GLFW_KEY_UNKNOWN) {
        return KeyCode::UNKNOWN;
    }
    
    // Direct mapping for most keys since GLFW uses similar values
    switch (glfw_key) {
        case GLFW_KEY_SPACE: return KeyCode::SPACE;
        case GLFW_KEY_APOSTROPHE: return KeyCode::APOSTROPHE;
        case GLFW_KEY_COMMA: return KeyCode::COMMA;
        case GLFW_KEY_MINUS: return KeyCode::MINUS;
        case GLFW_KEY_PERIOD: return KeyCode::PERIOD;
        case GLFW_KEY_SLASH: return KeyCode::SLASH;
        case GLFW_KEY_0: return KeyCode::KEY_0;
        case GLFW_KEY_1: return KeyCode::KEY_1;
        case GLFW_KEY_2: return KeyCode::KEY_2;
        case GLFW_KEY_3: return KeyCode::KEY_3;
        case GLFW_KEY_4: return KeyCode::KEY_4;
        case GLFW_KEY_5: return KeyCode::KEY_5;
        case GLFW_KEY_6: return KeyCode::KEY_6;
        case GLFW_KEY_7: return KeyCode::KEY_7;
        case GLFW_KEY_8: return KeyCode::KEY_8;
        case GLFW_KEY_9: return KeyCode::KEY_9;
        case GLFW_KEY_SEMICOLON: return KeyCode::SEMICOLON;
        case GLFW_KEY_EQUAL: return KeyCode::EQUAL;
        case GLFW_KEY_A: return KeyCode::A;
        case GLFW_KEY_B: return KeyCode::B;
        case GLFW_KEY_C: return KeyCode::C;
        case GLFW_KEY_D: return KeyCode::D;
        case GLFW_KEY_E: return KeyCode::E;
        case GLFW_KEY_F: return KeyCode::F;
        case GLFW_KEY_G: return KeyCode::G;
        case GLFW_KEY_H: return KeyCode::H;
        case GLFW_KEY_I: return KeyCode::I;
        case GLFW_KEY_J: return KeyCode::J;
        case GLFW_KEY_K: return KeyCode::K;
        case GLFW_KEY_L: return KeyCode::L;
        case GLFW_KEY_M: return KeyCode::M;
        case GLFW_KEY_N: return KeyCode::N;
        case GLFW_KEY_O: return KeyCode::O;
        case GLFW_KEY_P: return KeyCode::P;
        case GLFW_KEY_Q: return KeyCode::Q;
        case GLFW_KEY_R: return KeyCode::R;
        case GLFW_KEY_S: return KeyCode::S;
        case GLFW_KEY_T: return KeyCode::T;
        case GLFW_KEY_U: return KeyCode::U;
        case GLFW_KEY_V: return KeyCode::V;
        case GLFW_KEY_W: return KeyCode::W;
        case GLFW_KEY_X: return KeyCode::X;
        case GLFW_KEY_Y: return KeyCode::Y;
        case GLFW_KEY_Z: return KeyCode::Z;
        case GLFW_KEY_LEFT_BRACKET: return KeyCode::LEFT_BRACKET;
        case GLFW_KEY_BACKSLASH: return KeyCode::BACKSLASH;
        case GLFW_KEY_RIGHT_BRACKET: return KeyCode::RIGHT_BRACKET;
        case GLFW_KEY_GRAVE_ACCENT: return KeyCode::GRAVE_ACCENT;
        case GLFW_KEY_ESCAPE: return KeyCode::ESCAPE;
        case GLFW_KEY_ENTER: return KeyCode::ENTER;
        case GLFW_KEY_TAB: return KeyCode::TAB;
        case GLFW_KEY_BACKSPACE: return KeyCode::BACKSPACE;
        case GLFW_KEY_INSERT: return KeyCode::INSERT;
        case GLFW_KEY_DELETE: return KeyCode::DELETE;
        case GLFW_KEY_RIGHT: return KeyCode::RIGHT;
        case GLFW_KEY_LEFT: return KeyCode::LEFT;
        case GLFW_KEY_DOWN: return KeyCode::DOWN;
        case GLFW_KEY_UP: return KeyCode::UP;
        case GLFW_KEY_PAGE_UP: return KeyCode::PAGE_UP;
        case GLFW_KEY_PAGE_DOWN: return KeyCode::PAGE_DOWN;
        case GLFW_KEY_HOME: return KeyCode::HOME;
        case GLFW_KEY_END: return KeyCode::END;
        case GLFW_KEY_CAPS_LOCK: return KeyCode::CAPS_LOCK;
        case GLFW_KEY_SCROLL_LOCK: return KeyCode::SCROLL_LOCK;
        case GLFW_KEY_NUM_LOCK: return KeyCode::NUM_LOCK;
        case GLFW_KEY_PRINT_SCREEN: return KeyCode::PRINT_SCREEN;
        case GLFW_KEY_PAUSE: return KeyCode::PAUSE;
        case GLFW_KEY_F1: return KeyCode::F1;
        case GLFW_KEY_F2: return KeyCode::F2;
        case GLFW_KEY_F3: return KeyCode::F3;
        case GLFW_KEY_F4: return KeyCode::F4;
        case GLFW_KEY_F5: return KeyCode::F5;
        case GLFW_KEY_F6: return KeyCode::F6;
        case GLFW_KEY_F7: return KeyCode::F7;
        case GLFW_KEY_F8: return KeyCode::F8;
        case GLFW_KEY_F9: return KeyCode::F9;
        case GLFW_KEY_F10: return KeyCode::F10;
        case GLFW_KEY_F11: return KeyCode::F11;
        case GLFW_KEY_F12: return KeyCode::F12;
        case GLFW_KEY_LEFT_SHIFT: return KeyCode::LEFT_SHIFT;
        case GLFW_KEY_LEFT_CONTROL: return KeyCode::LEFT_CONTROL;
        case GLFW_KEY_LEFT_ALT: return KeyCode::LEFT_ALT;
        case GLFW_KEY_LEFT_SUPER: return KeyCode::LEFT_SUPER;
        case GLFW_KEY_RIGHT_SHIFT: return KeyCode::RIGHT_SHIFT;
        case GLFW_KEY_RIGHT_CONTROL: return KeyCode::RIGHT_CONTROL;
        case GLFW_KEY_RIGHT_ALT: return KeyCode::RIGHT_ALT;
        case GLFW_KEY_RIGHT_SUPER: return KeyCode::RIGHT_SUPER;
        default: return KeyCode::UNKNOWN;
    }
}

constexpr MouseButton glfw_button_to_mouse_button(int glfw_button) {
    switch (glfw_button) {
        case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::LEFT;
        case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::RIGHT;
        case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::MIDDLE;
        case GLFW_MOUSE_BUTTON_4: return MouseButton::BUTTON_4;
        case GLFW_MOUSE_BUTTON_5: return MouseButton::BUTTON_5;
        case GLFW_MOUSE_BUTTON_6: return MouseButton::BUTTON_6;
        case GLFW_MOUSE_BUTTON_7: return MouseButton::BUTTON_7;
        case GLFW_MOUSE_BUTTON_8: return MouseButton::BUTTON_8;
        default: return MouseButton::LEFT;
    }
}

constexpr KeyAction glfw_action_to_key_action(int glfw_action) {
    switch (glfw_action) {
        case GLFW_RELEASE: return KeyAction::RELEASE;
        case GLFW_PRESS: return KeyAction::PRESS;
        case GLFW_REPEAT: return KeyAction::REPEAT;
        default: return KeyAction::RELEASE;
    }
}

constexpr MouseAction glfw_action_to_mouse_action(int glfw_action) {
    switch (glfw_action) {
        case GLFW_RELEASE: return MouseAction::RELEASE;
        case GLFW_PRESS: return MouseAction::PRESS;
        default: return MouseAction::RELEASE;
    }
}

constexpr KeyModifier glfw_mods_to_key_modifier(int glfw_mods) {
    KeyModifier modifiers = KeyModifier::NONE;
    
    if (glfw_mods & GLFW_MOD_SHIFT) {
        modifiers = modifiers | KeyModifier::SHIFT;
    }
    if (glfw_mods & GLFW_MOD_CONTROL) {
        modifiers = modifiers | KeyModifier::CONTROL;
    }
    if (glfw_mods & GLFW_MOD_ALT) {
        modifiers = modifiers | KeyModifier::ALT;
    }
    if (glfw_mods & GLFW_MOD_SUPER) {
        modifiers = modifiers | KeyModifier::SUPER;
    }
    if (glfw_mods & GLFW_MOD_CAPS_LOCK) {
        modifiers = modifiers | KeyModifier::CAPS_LOCK;
    }
    if (glfw_mods & GLFW_MOD_NUM_LOCK) {
        modifiers = modifiers | KeyModifier::NUM_LOCK;
    }
    
    return modifiers;
}

constexpr KeyEvent create_key_event(int key, int scancode, int action, int mods) {
    return KeyEvent{
        .key_code = glfw_key_to_keycode(key),
        .scan_code = static_cast<std::int16_t>(scancode),
        .action = glfw_action_to_key_action(action),
        .modifiers = glfw_mods_to_key_modifier(mods)
    };
}

constexpr MouseButtonEvent create_mouse_button_event(int button, int action, int mods, double x, double y) {
    return MouseButtonEvent{
        .button = glfw_button_to_mouse_button(button),
        .action = glfw_action_to_mouse_action(action),
        .modifiers = glfw_mods_to_key_modifier(mods),
        .padding = 0,
        .x = x,
        .y = y
    };
}

constexpr MouseScrollEvent create_mouse_scroll_event(double xoffset, double yoffset, double x, double y, int mods) {
    return MouseScrollEvent{
        .x_offset = xoffset,
        .y_offset = yoffset,
        .x = x,
        .y = y,
        .modifiers = glfw_mods_to_key_modifier(mods),
        .padding = {}
    };
}

}