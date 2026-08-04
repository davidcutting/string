#pragma once

namespace string::client
{

// UI sound-cue hooks (brief 05 M5). No-op until audio lands (a later brief): the widget kit calls
// these on interaction so the wiring exists and later just swaps the body. Cheap enough to call
// unconditionally; a real backend would debounce/pool.
enum class Cue
{
    Hover,
    Press,
    Activate,
    Focus,
    Error,
};

inline void play_cue(Cue /*cue*/)
{
    // no-op: audio subsystem not yet present. Intentionally empty (see brief 05 M5).
}

}  // namespace string::client
