#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>

namespace sandbox::ui
{

float ease(Curve curve, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (curve)
    {
        case Curve::LINEAR:      return t;
        case Curve::EASE_IN:     return t * t * t;
        case Curve::EASE_OUT:    { const float u = 1.0f - t; return 1.0f - u * u * u; }
        case Curve::EASE_IN_OUT: return t < 0.5f ? 4.0f * t * t * t
                                                 : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        case Curve::EASE_OUT_BACK:
        {
            const float c1 = 1.70158f;
            const float c3 = c1 + 1.0f;
            const float u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case Curve::SPRING: return t;  // integrated by the table, not a t-curve
    }
    return t;
}

void Motion::begin_frame(float dt)
{
    dt_ = std::clamp(dt, 0.0f, 0.1f);  // clamp so a hitch/first frame can't jump animations
    for (auto& [k, e] : entries_)
        e.seen = false;
}

void Motion::end_frame()
{
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (!it->second.seen)
            it = entries_.erase(it);
        else
            ++it;
    }
}

float Motion::animate(std::uint64_t id, Prop prop, float target, const Transition& t)
{
    Entry& e = entries_[key(id, prop)];
    e.seen = true;
    if (!e.live)
    {
        // First appearance: snap to target so a newly-shown element doesn't slide in from 0.
        e.current = target;
        e.target = target;
        e.velocity = 0.0f;
        e.live = true;
        return e.current;
    }
    e.target = target;

    if (t.curve == Curve::SPRING)
    {
        // Semi-implicit spring: stiffness from duration (shorter = stiffer), critical-ish damping.
        const float omega = 8.0f / std::max(0.03f, t.duration);
        const float k = omega * omega;
        const float c = 2.0f * omega;  // ~critical damping
        const float accel = k * (e.target - e.current) - c * e.velocity;
        e.velocity += accel * dt_;
        e.current += e.velocity * dt_;
        if (std::abs(e.target - e.current) < 0.05f && std::abs(e.velocity) < 0.05f)
        {
            e.current = e.target;
            e.velocity = 0.0f;
        }
        return e.current;
    }

    // Duration-based ease: advance a normalized progress toward the target. We track progress
    // implicitly by moving `current` a fraction of the remaining distance shaped by the curve's
    // local slope — a simple, stable approximation that reads as the declared curve for UI glides.
    if (e.current == e.target)
        return e.current;
    const float step = t.duration <= 0.0f ? 1.0f : std::clamp(dt_ / t.duration, 0.0f, 1.0f);
    // Blend factor shaped by the curve so EASE_OUT decelerates near the target, etc.
    const float shaped = ease(t.curve, step);
    e.current += (e.target - e.current) * shaped;
    if (std::abs(e.target - e.current) < 0.25f)
        e.current = e.target;
    return e.current;
}

string::color Motion::animate_color(std::uint64_t id, string::color target, const Transition& t)
{
    const float r = animate(id, Prop::R, target.r, t);
    const float g = animate(id, Prop::G, target.g, t);
    const float b = animate(id, Prop::B, target.b, t);
    const float a = animate(id, Prop::A, target.a, t);
    auto u8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f)); };
    return string::color{ u8(r), u8(g), u8(b), u8(a) };
}

}  // namespace sandbox::ui
