#include <string/scene/animator.hpp>

#include <string/core/logger.hpp>

namespace string::scene
{

using ::string::anim::AnimSet;
using ::string::anim::Clip;

animator::animator(std::shared_ptr<AnimSet> set, assets::skin_id skin,
                   uint64_t expected_skeleton_hash)
: set_(std::move(set))
, skin_(skin)
{
    if (set_ == nullptr) return;   // load already WARNed; bind pose
    if (set_->skeleton_hash() != expected_skeleton_hash)
    {
        STRING_LOG_WARN("[anim] skeleton hash mismatch (stale pack?) — bind pose");
        set_.reset();
        return;
    }
    player_.emplace(set_->skeleton());
    if (const Clip* first = default_clip()) fade_.play(first, 0.0f);
}

const Clip* animator::default_clip() const
{
    if (set_ == nullptr) return nullptr;
    if (const Clip* idle = set_->find("Idle_Loop")) return idle;
    return set_->clips().empty() ? nullptr : set_->clips().front().get();
}

void animator::play(const Clip* clip, float fade_seconds)
{
    if (valid() && clip != nullptr) fade_.play(clip, fade_seconds);
}

bool animator::play(std::string_view clip, float fade_seconds)
{
    if (!valid()) return false;
    const Clip* found = set_->find(clip);
    if (found == nullptr) return false;
    fade_.play(found, fade_seconds);
    return true;
}

void animator::tick(float dt, float rate)
{
    if (!valid()) return;
    fade_.advance(dt, rate);
    player_->set_layers(fade_.layers());
    player_->sample();
}

std::span<const glm::mat4> animator::model_space() const
{
    return valid() ? player_->model_space() : std::span<const glm::mat4>{};
}

}  // namespace string::scene
