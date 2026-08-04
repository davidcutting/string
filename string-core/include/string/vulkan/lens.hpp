#pragma once

#include <cstdint>

namespace String
{

// Brief 14 M5 — the lens: a screen region the composite substitutes, either magnified (a pixel
// loupe) or sourced from a different texture (a debug target shown in place).
//
// DIRECTION OF FLOW IS UI -> PASS, the opposite of GraphIntrospect/GpuProfiler. Those publish engine
// numbers for the UI to read; this is the UI telling the pass what to do. That is why it is a
// separate, explicitly MUTABLE store rather than a second use of the read-only global handle — the
// brief warns not to assume that channel is bidirectional, and it is not.
//
// Single-threaded by construction: the debug UI authors on the render thread and the composite reads
// on the render thread, in that order within a frame. No synchronisation, for the same reason
// MeshOverlayStats needs none.
struct Lens
{
    // Screen-space rect in pixels. Held as the SAME shape a panel uses, because the rect is driven
    // by `update_panel` — a lens IS a movable, resizable rect, so it inherits the origin-capture
    // anti-drift discipline that took several corrections to get right on panels.
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    // 1 = exact correspondence (the sampling reduces to identity, structurally rather than by
    // tuning). Above 1 it is a pixel loupe.
    float magnification = 1.0f;
    // Bindless texture slot to sample. 0 = the scene itself, which is the only source available
    // until render-target sampling lands (see the image widget's deferred half) — the field exists
    // now because the LAYOUT is the expensive part to retrofit, not the UI.
    std::uint32_t source_slot = 0;
    // Debug targets carry raw data (normals, roughness), so applying exposure and the tonemap to
    // them produces a plausible-looking WRONG image. Same discipline as the UI cancelling exposure
    // to stay display-referred.
    bool bypass_tonemap = false;
};

class LensState
{
public:
    // Capped, and the cap is load-bearing: the shader cost is a bounded loop over `count`, which is
    // only acceptable because the bound is small and known at compile time.
    static constexpr std::uint32_t kMaxLenses = 4;

    std::uint32_t count = 0;
    Lens lenses[kMaxLenses]{};

    // The one instance. Mutable on purpose (see the note above).
    static LensState& instance();
};

}  // namespace String
