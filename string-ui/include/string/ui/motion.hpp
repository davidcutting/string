#pragma once

#include <cstdint>
#include <unordered_map>

#include <string/ui/layout.hpp>

// UI motion (promoted to the engine in brief 12 M0b; originally sandbox/ui/motion.hpp, brief 05).
// Engine-side because the panel layer above it — dock drags, resize handles, tab transitions —
// needs it, and because it has no app knowledge: it is a state table keyed by element id.
namespace string::ui
{

// Easing curves for declared transitions. SPRING is handled specially by the animation table
// (velocity-based), the rest are pure f(t) on t in [0,1].
enum class Curve : std::uint8_t
{
    LINEAR,
    EASE_IN,       // cubic in
    EASE_OUT,      // cubic out
    EASE_IN_OUT,   // cubic in-out
    EASE_OUT_BACK, // slight overshoot then settle (satisfying "pop")
    SPRING,        // critically-ish damped spring toward the target
};

// A one-line transition declaration: how fast, what shape. `duration` is seconds to traverse; for
// SPRING it maps to stiffness. Authors pass this to Motion::animate.
struct Transition
{
    float duration = 0.15f;
    Curve curve = Curve::EASE_OUT;
    // Seconds to HOLD before easing. The clock starts when the TARGET IS WRITTEN — not when the
    // element appears and not every frame — so the value stays where it is for `delay`, then eases
    // over `duration` with `curve`. Writing a new target during the hold restarts it, which is what
    // "the clock starts when the target is written" has to mean for a state that changes its mind.
    //
    // STAGGER NEEDS NO API: it is author-side index math, `{.delay = i * 0.03f}` in the loop that
    // authors the rows. That is why this is a field rather than a choreography system — the
    // pull-based model already gives every element its own clock, so a sequencer would be a second
    // way to say something the author can say directly.
    //
    // Deliberately NOT applied on first sight: a newly-seen entry still snaps to its target, so a
    // delayed ENTRANCE is expressed by authoring the initial state and then the final one, exactly
    // as it was before this field existed.
    //
    // At the default 0 this costs nothing and changes nothing: the hold branch is only ever entered
    // by an author who asked for it.
    float delay = 0.0f;
};

// Evaluate an easing curve f: [0,1] -> value (may exceed [0,1] for EASE_OUT_BACK). SPRING is not a
// closed-form t-curve; the table integrates it, so this returns t for SPRING (unused there).
float ease(Curve curve, float t);

// Animation state table: the layout tree is rebuilt every frame, so per-element animated values are
// keyed by a stable (element id, property) hash and interpolated toward the author's declared
// target here. The author asks for the CURRENT value to apply to the element this frame; over
// frames it glides to the target with the declared duration/curve.
//
//   float a = motion.animate(make_id("panel").hash, Prop::A, focused ? 255 : 120, {0.2f});
//   panel.color.a = uint8_t(a);
//
// One Motion lives in the author closure (persists across frames). Entries untouched for a frame are
// aged out so a rebuilt/removed element doesn't leak state.
class Motion
{
public:
    // Named animatable channels. A single element can animate several independently (x/y/w/h/rgba).
    enum class Prop : std::uint8_t { X, Y, W, H, R, G, B, A, Scale, Custom0, Custom1, Custom2 };

    // Advance every live entry by `dt` and mark all as stale (animate() un-stales the ones touched
    // this frame). Call once at the top of the author before authoring.
    void begin_frame(float dt);
    // Drop entries not touched since the last begin_frame (removed/hidden elements don't leak).
    void end_frame();

    // Return the current animated value for (id, prop), gliding toward `target` per `t`. First sight
    // of a key snaps to the target (no spurious slide-in from 0 on the first frame it appears).
    float animate(std::uint64_t id, Prop prop, float target, const Transition& t);

    // Convenience: animate a color's four channels toward `target` with one transition. Returns the
    // eased colour to assign. `id` should be the element's stable id hash.
    color animate_color(std::uint64_t id, color target, const Transition& t);

    // Live entry count — for tests and the debug inspector (brief 14): a leaking author shows up
    // here as a count that climbs without bound.
    [[nodiscard]] std::size_t live_count() const noexcept { return entries_.size(); }

private:
    struct Entry
    {
        float current = 0.0f;
        float target = 0.0f;
        float velocity = 0.0f;  // for SPRING
        float hold = 0.0f;      // seconds of Transition::delay left before easing resumes
        bool seen = false;
        bool live = false;
    };
    static std::uint64_t key(std::uint64_t id, Prop prop)
    {
        return id * 12ull + static_cast<std::uint64_t>(prop);
    }

    std::unordered_map<std::uint64_t, Entry> entries_;
    float dt_ = 0.0f;
};

}  // namespace string::ui
