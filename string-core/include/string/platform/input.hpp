#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

namespace string
{

enum class KeyCode : std::uint16_t
{
    UNKNOWN = 0,
    SPACE = 32,
    APOSTROPHE = 39,
    COMMA = 44,
    MINUS = 45,
    PERIOD = 46,
    SLASH = 47,
    KEY_0 = 48, KEY_1 = 49, KEY_2 = 50, KEY_3 = 51, KEY_4 = 52,
    KEY_5 = 53, KEY_6 = 54, KEY_7 = 55, KEY_8 = 56, KEY_9 = 57,
    SEMICOLON = 59,
    EQUAL = 61,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72,
    I = 73, J = 74, K = 75, L = 76, M = 77, N = 78, O = 79, P = 80,
    Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86, W = 87, X = 88,
    Y = 89, Z = 90,
    LEFT_BRACKET = 91,
    BACKSLASH = 92,
    RIGHT_BRACKET = 93,
    GRAVE_ACCENT = 96,
    ESCAPE = 256,
    ENTER = 257,
    TAB = 258,
    BACKSPACE = 259,
    INSERT = 260,
    DELETE = 261,
    RIGHT = 262, LEFT = 263, DOWN = 264, UP = 265,
    PAGE_UP = 266,
    PAGE_DOWN = 267,
    HOME = 268,
    END = 269,
    CAPS_LOCK = 280,
    SCROLL_LOCK = 281,
    NUM_LOCK = 282,
    PRINT_SCREEN = 283,
    PAUSE = 284,
    F1 = 290, F2 = 291, F3 = 292, F4 = 293, F5 = 294, F6 = 295,
    F7 = 296, F8 = 297, F9 = 298, F10 = 299, F11 = 300, F12 = 301,
    LEFT_SHIFT = 340,
    LEFT_CONTROL = 341,
    LEFT_ALT = 342,
    LEFT_SUPER = 343,
    RIGHT_SHIFT = 344,
    RIGHT_CONTROL = 345,
    RIGHT_ALT = 346,
    RIGHT_SUPER = 347
};


enum class MouseButton : std::uint8_t
{
    LEFT = 0,
    RIGHT = 1,
    MIDDLE = 2,
    BUTTON_4 = 3,
    BUTTON_5 = 4,
    BUTTON_6 = 5,
    BUTTON_7 = 6,
    BUTTON_8 = 7
};

// Gamepad face/d-pad/shoulder buttons + stick clicks (brief 05: controller-first UI). Values follow
// SDL3's SDL_GamepadButton ordering so the backend maps 1:1. LEFT/RIGHT/UP/DOWN are the d-pad.
enum class GamepadButton : std::uint8_t
{
    A = 0, B = 1, X = 2, Y = 3,
    BACK = 4, GUIDE = 5, START = 6,
    LEFT_STICK = 7, RIGHT_STICK = 8,
    LEFT_SHOULDER = 9, RIGHT_SHOULDER = 10,
    UP = 11, DOWN = 12, LEFT = 13, RIGHT = 14,
    COUNT = 15
};

// Gamepad analog axes (SDL3 order): sticks (normalized -1..1) + triggers (0..1).
enum class GamepadAxis : std::uint8_t
{
    LEFT_X = 0, LEFT_Y = 1, RIGHT_X = 2, RIGHT_Y = 3,
    LEFT_TRIGGER = 4, RIGHT_TRIGGER = 5,
    COUNT = 6
};

// --- Human-readable names, for a rebinding UI -------------------------------------------------
//
// A binding list has to SHOW what a key is, and an enum value is not that. Declared beside the enums
// so a new code and its name stay one edit apart; anything unnamed falls back to "key <n>" rather
// than to an empty cell, so an unmapped code reads as a gap instead of as "unbound".
[[nodiscard]] std::string_view key_name(KeyCode k) noexcept;
[[nodiscard]] std::string_view button_name(MouseButton b) noexcept;
[[nodiscard]] std::string_view button_name(GamepadButton b) noexcept;

enum class KeyAction : std::uint8_t
{
    RELEASE = 0,
    PRESS = 1,
    REPEAT = 2
};

constexpr bool is_pressed(const KeyAction& key_action)
{
    return key_action == KeyAction::PRESS;
}

constexpr bool is_released(const KeyAction& key_action)
{
    return key_action == KeyAction::RELEASE;
}

constexpr bool is_repeat(const KeyAction& key_action)
{
    return key_action == KeyAction::REPEAT;
}

enum class MouseAction : std::uint8_t
{
    RELEASE = 0,
    PRESS = 1
};

constexpr bool is_pressed(const MouseAction& mouse_action)
{
    return mouse_action == MouseAction::PRESS;
}

constexpr bool is_released(const MouseAction& mouse_action)
{
    return mouse_action == MouseAction::RELEASE;
}

// Modifier flags (can be combined with bitwise OR)
enum class KeyModifier : std::uint8_t
{
    NONE = 0x00,
    SHIFT = 0x01,
    CONTROL = 0x02,
    ALT = 0x04,
    SUPER = 0x08,
    CAPS_LOCK = 0x10,
    NUM_LOCK = 0x20
};

constexpr KeyModifier operator|(KeyModifier a, KeyModifier b)
{
    return static_cast<KeyModifier>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr KeyModifier operator&(KeyModifier a, KeyModifier b)
{
    return static_cast<KeyModifier>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr bool has_modifier(KeyModifier modifiers, KeyModifier flag)
{
    return (modifiers & flag) != KeyModifier::NONE;
}

// Per-frame input state, polled by consumers (e.g. a camera in a pass's update()). The WSI
// backend fills it each frame from platform events: key/button state persists across frames
// (set on press, cleared on release); the mouse delta accumulates within a frame and is reset
// by new_frame() at the top of each window update. `mouse_captured` reflects relative-mouse
// (look) mode — deltas are only accumulated while captured, so a released cursor stops the look.
class Input
{
public:
    // Held: key/button is currently down. Pressed/released: the down-state changed *this frame*
    // (edge), computed against the previous frame's snapshot taken in new_frame(). Edge queries
    // are what one-shot actions (jump, toggle) want; held is for continuous ones (movement).
    bool key_down(KeyCode key) const { return keys_[static_cast<std::size_t>(key)]; }
    bool key_pressed(KeyCode key) const
    {
        const std::size_t i = static_cast<std::size_t>(key);
        return keys_[i] && !prev_keys_[i];
    }
    bool key_released(KeyCode key) const
    {
        const std::size_t i = static_cast<std::size_t>(key);
        return !keys_[i] && prev_keys_[i];
    }
    // A held key AUTO-REPEATING this frame, as the PLATFORM decides — its delay and rate are an OS
    // accessibility setting, and a UI that invents its own is wrong for the people who changed it.
    bool key_repeat(KeyCode key) const { return repeat_keys_[static_cast<std::size_t>(key)]; }
    // The edge text editing wants: holding Left should walk the caret, not move it once.
    bool key_edit(KeyCode key) const { return key_pressed(key) || key_repeat(key); }
    // The system clipboard, cached by the WSI backend and refreshed when the platform says it
    // changed — NOT read per frame, which would be a syscall and an allocation every frame for a
    // value that changes almost never.
    std::string_view clipboard_text() const { return clipboard_; }
    bool mouse_button_down(MouseButton button) const
    {
        return buttons_[static_cast<std::size_t>(button)];
    }
    bool mouse_button_pressed(MouseButton button) const
    {
        const std::size_t i = static_cast<std::size_t>(button);
        return buttons_[i] && !prev_buttons_[i];
    }
    bool mouse_button_released(MouseButton button) const
    {
        const std::size_t i = static_cast<std::size_t>(button);
        return !buttons_[i] && prev_buttons_[i];
    }
    glm::vec2 mouse_delta() const { return mouse_delta_; }
    bool mouse_captured() const { return mouse_captured_; }

    // The first key whose press edge landed this frame, or UNKNOWN. For a REBINDING UI and nothing
    // else: "bind the next key I hit" is the one question that is genuinely about a key rather than
    // an action, so it cannot go through InputMap without inverting the whole point of that layer.
    KeyCode first_key_pressed() const noexcept
    {
        for (std::size_t i = 0; i < keys_.size(); ++i)
            if (keys_[i] && !prev_keys_[i]) return static_cast<KeyCode>(i);
        return KeyCode::UNKNOWN;
    }
    // Desired capture state (game vs UI mode), set by the app (a UI pass) and reconciled by the WSI
    // backend each frame into the actual mouse_captured() state. Lets the UI decide when a click
    // frees/grabs the cursor (it gets first dibs on a click) rather than the window guessing.
    bool capture_requested() const { return capture_requested_; }
    void set_capture_requested(bool requested) { capture_requested_ = requested; }
    // While a modal text surface (the debug console, a chat bar) is open it sets this, and gameplay
    // input routed through InputMap is suppressed — the surface reads raw Input::key_*/typed_text()
    // directly, so it still receives keys. Reconciled each frame by the surface: it must clear this
    // when it closes.
    //
    // This is no longer a special case inside InputMap: it raises the implicit
    // `InputMap::text_context()`, which sits directly above the base context and claims everything
    // below it. Contexts the app pushes above base are unaffected — the flag suppresses GAMEPLAY,
    // which is what it has always said it does. Text itself is deliberately NOT an action (you do
    // not rebind 'e' to mean 'e'), which is why it lives here on raw input rather than as a binding.
    bool text_capture() const { return text_capture_; }
    void set_text_capture(bool capture) { text_capture_ = capture; }
    // Absolute cursor position in window pixels (top-left origin), for UI hit-testing. Unlike the
    // delta this persists across frames (last known position), and is tracked even while captured.
    glm::vec2 mouse_position() const { return mouse_position_; }
    // Unicode text typed *this frame* as UTF-8 (empty if none), for text fields. Distinct from
    // key_* state: this is composed character input (honours layout, shift, IME), cleared each
    // new_frame(). Backspace/enter/etc. are not here — read those via key_pressed().
    std::string_view typed_text() const { return typed_text_; }

    // Mouse wheel movement THIS FRAME, in notches (positive = away from the user / scroll up).
    // Accumulated because several wheel events can arrive between frames, and cleared in new_frame
    // like typed text — both are per-frame EVENTS rather than state, so a consumer that misses a
    // frame should miss the input rather than see it twice.
    float scroll_y() const { return scroll_y_; }

    // --- Gamepad (brief 05: controller-first UI) ---
    bool gamepad_connected() const { return gamepad_connected_; }
    bool gamepad_down(GamepadButton b) const { return pad_buttons_[static_cast<std::size_t>(b)]; }
    bool gamepad_pressed(GamepadButton b) const
    {
        const std::size_t i = static_cast<std::size_t>(b);
        return pad_buttons_[i] && !prev_pad_buttons_[i];
    }
    bool gamepad_released(GamepadButton b) const
    {
        const std::size_t i = static_cast<std::size_t>(b);
        return !pad_buttons_[i] && prev_pad_buttons_[i];
    }
    float gamepad_axis(GamepadAxis a) const { return pad_axes_[static_cast<std::size_t>(a)]; }

    // --- Filled by the WSI backend ---
    void set_gamepad_connected(bool connected) { gamepad_connected_ = connected; }
    void set_gamepad_button(GamepadButton b, bool down)
    {
        pad_buttons_[static_cast<std::size_t>(b)] = down;
    }
    void set_gamepad_axis(GamepadAxis a, float value)
    {
        pad_axes_[static_cast<std::size_t>(a)] = value;
    }
    void set_key(KeyCode key, bool down) { keys_[static_cast<std::size_t>(key)] = down; }
    void set_key_repeat(KeyCode key) { repeat_keys_[static_cast<std::size_t>(key)] = true; }
    void set_clipboard_text(std::string text) { clipboard_ = std::move(text); }
    // A copy/cut REQUEST from the UI, serviced by the WSI backend next update — the same shape as
    // capture_requested, and for the same reason: the UI must not need a window handle to copy.
    void request_clipboard_write(std::string text)
    {
        clipboard_write_ = std::move(text);
        clipboard_write_pending_ = true;
    }
    bool take_clipboard_write(std::string& out)
    {
        if (!clipboard_write_pending_) return false;
        out = std::move(clipboard_write_);
        clipboard_write_.clear();
        clipboard_write_pending_ = false;
        return true;
    }
    void set_mouse_button(MouseButton button, bool down)
    {
        buttons_[static_cast<std::size_t>(button)] = down;
    }
    void add_mouse_delta(glm::vec2 delta) { mouse_delta_ += delta; }
    void set_mouse_position(glm::vec2 position) { mouse_position_ = position; }
    void set_mouse_captured(bool captured) { mouse_captured_ = captured; }
    void add_typed_text(std::string_view utf8) { typed_text_ += utf8; }
    void add_scroll(float y) { scroll_y_ += y; }
    // Called at the top of each window update, *before* polling events: snapshots the current
    // key/button state as "previous" (so pressed/released can detect this frame's edges) and
    // resets the per-frame accumulators (mouse delta, typed text).
    void new_frame()
    {
        repeat_keys_.fill(false);   // a repeat is an EVENT for one frame, like typed text
        prev_keys_ = keys_;
        prev_buttons_ = buttons_;
        prev_pad_buttons_ = pad_buttons_;
        mouse_delta_ = glm::vec2(0.0f);
        typed_text_.clear();
        scroll_y_ = 0.0f;
    }

private:
    // KeyCode values are sparse GLFW codes topping out at 347; a flat array covers them.
    static constexpr std::size_t MAX_KEYS = 512;
    std::array<bool, MAX_KEYS> keys_{};
    std::array<bool, MAX_KEYS> prev_keys_{};
    std::array<bool, MAX_KEYS> repeat_keys_{};
    std::string clipboard_{};
    std::string clipboard_write_{};
    bool clipboard_write_pending_ = false;
    std::array<bool, 8> buttons_{};
    std::array<bool, 8> prev_buttons_{};
    glm::vec2 mouse_delta_{ 0.0f };
    glm::vec2 mouse_position_{ 0.0f };
    bool mouse_captured_ = false;
    bool capture_requested_ = true;   // start in game mode (mouse-look)
    bool text_capture_ = false;       // brief 06: modal console open -> suppress InputMap actions
    std::string typed_text_;
    float scroll_y_ = 0.0f;

    static constexpr std::size_t PAD_BUTTONS = static_cast<std::size_t>(GamepadButton::COUNT);
    static constexpr std::size_t PAD_AXES = static_cast<std::size_t>(GamepadAxis::COUNT);
    std::array<bool, PAD_BUTTONS> pad_buttons_{};
    std::array<bool, PAD_BUTTONS> prev_pad_buttons_{};
    std::array<float, PAD_AXES> pad_axes_{};
    bool gamepad_connected_ = false;
};

}