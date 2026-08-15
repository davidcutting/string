#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <string/anim/anim.hpp>

#include <string/scene/asset_handles.hpp>

namespace string::scene
{

// Per-(entity, skin) playback state: the deduped AnimSet + an AnimPlayer + a CrossFade. Playback
// is WORLD state (what is this character doing) — the palette WRITE into the GPU ring stays
// renderer-side (the bridge owns the windows). Keeps brief 23's boundary: the engine samples and
// blends; state machines are game code — play() is the most a client gets.
//
// A pack that failed to load, or whose skeleton hash mismatches the skin, leaves the animator
// INVALID: the character renders bind pose (the ring's identity default), never garbage.
class animator
{
public:
    animator(std::shared_ptr<::string::anim::AnimSet> set, assets::skin_id skin,
             uint64_t expected_skeleton_hash);

    bool valid() const { return player_.has_value(); }
    assets::skin_id skin() const { return skin_; }
    const ::string::anim::AnimSet* set() const { return set_.get(); }

    // Cross-fade to a clip. The string overload returns false (and does nothing) when the pack has
    // no such clip — the caller decides whether that is a warning or a fallback.
    void play(const ::string::anim::Clip* clip, float fade_seconds);
    bool play(std::string_view clip, float fade_seconds);

    // Advance the fade + sample the blended pose (called by world::tick). `rate` scales time.
    void tick(float dt, float rate);

    // Model-space joint matrices in ozz joint order (valid after the first tick).
    std::span<const glm::mat4> model_space() const;

    // Idle_Loop when the pack has it (the UAL library convention), else the first clip.
    const ::string::anim::Clip* default_clip() const;

private:
    std::shared_ptr<::string::anim::AnimSet> set_;
    assets::skin_id skin_{};
    std::optional<::string::anim::AnimPlayer> player_;
    ::string::anim::CrossFade fade_;
};

}  // namespace string::scene
