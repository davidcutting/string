#pragma once

#include <cstdint>
#include <functional>
#include <string/platform/input.hpp>
#include <cmath>

namespace string
{

struct KeyEvent
{
    KeyCode key_code;           // 2 bytes
    std::int16_t scan_code;     // 2 bytes
    KeyAction action;           // 1 byte
    KeyModifier modifiers;      // 1 byte
};

constexpr bool is_pressed(const KeyEvent& key_event)
{
    return is_pressed(key_event.action);
}

constexpr bool is_released(const KeyEvent& key_event)
{
    return is_released(key_event.action);
}

constexpr bool is_repeat(const KeyEvent& key_event)
{
    return is_repeat(key_event.action);
}

struct MouseButtonEvent
{
    MouseButton button;         // 1 byte
    MouseAction action;         // 1 byte
    KeyModifier modifiers;      // 1 byte
    std::uint8_t padding;       // 1 byte padding for alignment
    double x;                   // 8 bytes
    double y;                   // 8 bytes
};

constexpr bool is_pressed(const MouseButtonEvent& mouse_button_event)
{
    return is_pressed(mouse_button_event.action);
}

constexpr bool is_released(const MouseButtonEvent& mouse_button_event)
{
    return is_released(mouse_button_event.action);
}

struct MouseMoveEvent
{
    double x;
    double y;
    double dx;
    double dy;
};

constexpr double get_distance(const MouseMoveEvent& mouse_move)
{
    return std::sqrt(mouse_move.dx * mouse_move.dx + mouse_move.dy * mouse_move.dy);
}

struct MouseScrollEvent
{
    double x_offset;            // 8 bytes
    double y_offset;            // 8 bytes
    double x;                   // 8 bytes - Mouse position at time of scroll
    double y;                   // 8 bytes
    KeyModifier modifiers;      // 1 byte
    std::uint8_t padding[7];    // 7 bytes padding for alignment
};

using OnKeyCallback = std::function<void(const KeyEvent&)>;
using OnMouseButtonCallback = std::function<void(const MouseButtonEvent&)>;
using OnMouseMoveCallback = std::function<void(const MouseMoveEvent&)>;
using OnMouseScrollCallback = std::function<void(const MouseScrollEvent&)>;

struct WindowResizeEvent
{
    uint32_t width;
    uint32_t height;
    uint32_t old_width;
    uint32_t old_height;
    
    bool is_size_increase() const { 
        return (width * height) > (old_width * old_height); 
    }
    
    float get_aspect_ratio() const { 
        return height > 0 ? static_cast<float>(width) / height : 0.0f; 
    }
};

struct WindowCloseEvent
{
    bool user_initiated = true;  // Was close initiated by user or programmatically
};

struct WindowMinimizeEvent
{
    bool minimized;  // true = minimized, false = restored
};

struct WindowMaximizeEvent
{
    bool maximized;  // true = maximized, false = restored
};

struct WindowFocusEvent
{
    bool gained_focus;
};

struct WindowMoveEvent
{
    int32_t x;
    int32_t y;
    int32_t old_x;
    int32_t old_y;
};

using OnWindowResizeCallback = std::function<void(const WindowResizeEvent&)>;
using OnWindowCloseCallback = std::function<void(const WindowCloseEvent&)>;
using OnWindowMinimizeCallback = std::function<void(const WindowMinimizeEvent&)>;
using OnWindowMaximizeCallback = std::function<void(const WindowMaximizeEvent&)>;
using OnWindowFocusCallback = std::function<void(const WindowFocusEvent&)>;
using OnWindowMoveCallback = std::function<void(const WindowMoveEvent&)>;

}