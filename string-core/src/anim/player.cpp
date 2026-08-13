#include <string/anim/anim.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <string/core/logger.hpp>

#include "ozz_impl.hpp"

namespace string::anim
{

// --- AnimPlayer -------------------------------------------------------------------------------

struct AnimPlayer::Impl
{
    std::shared_ptr<Skeleton> skeleton;
    std::vector<Layer> layers;

    // Reused across frames. ozz::vector for the SoA buffers — their 16-byte alignment is the
    // ozz allocator's job, std::vector's default allocator gives no such guarantee.
    // One sampling context per layer slot, resized to the skeleton and INVALIDATED when the
    // slot's clip changes (contexts cache decompressed keys per animation).
    std::vector<std::unique_ptr<ozz::animation::SamplingJob::Context>> contexts;
    std::vector<const Clip*> context_clips;
    std::vector<ozz::vector<ozz::math::SoaTransform>> layer_locals;
    ozz::vector<ozz::math::SoaTransform> blended;
    ozz::vector<ozz::math::Float4x4> models;
    std::vector<glm::mat4> glm_models;

    void sample_layer(std::size_t index, ozz::vector<ozz::math::SoaTransform>& out)
    {
        const Layer& layer = layers[index];
        if (contexts.size() <= index)
        {
            contexts.resize(index + 1);
            context_clips.resize(index + 1, nullptr);
        }
        if (!contexts[index])
            contexts[index] = std::make_unique<ozz::animation::SamplingJob::Context>(
                skeleton->impl().skeleton.num_joints());
        if (context_clips[index] != layer.clip)
        {
            contexts[index]->Invalidate();
            context_clips[index] = layer.clip;
        }
        const ozz::animation::Animation& animation = layer.clip->impl().animation;
        ozz::animation::SamplingJob job;
        job.animation = &animation;
        job.context = contexts[index].get();
        job.ratio = animation.duration() > 0.0f ? layer.time / animation.duration() : 0.0f;
        job.output = ozz::make_span(out);
        if (!job.Run()) STRING_LOG_WARN("[anim] SamplingJob failed for clip '{}'", layer.clip->name());
    }
};

AnimPlayer::AnimPlayer(std::shared_ptr<Skeleton> skeleton) : impl_(std::make_unique<Impl>())
{
    impl_->skeleton = std::move(skeleton);
    const int soa = impl_->skeleton->impl().skeleton.num_soa_joints();
    const int joints = impl_->skeleton->impl().skeleton.num_joints();
    impl_->blended.resize(soa);
    impl_->models.resize(joints);
    impl_->glm_models.resize(joints);
}

AnimPlayer::~AnimPlayer() = default;
AnimPlayer::AnimPlayer(AnimPlayer&&) noexcept = default;
AnimPlayer& AnimPlayer::operator=(AnimPlayer&&) noexcept = default;

void AnimPlayer::set_layers(std::span<const Layer> layers)
{
    impl_->layers.assign(layers.begin(), layers.end());
    // Drop layers that cannot sample rather than making every frame check.
    std::erase_if(impl_->layers, [](const Layer& l) { return l.clip == nullptr || l.weight <= 0.0f; });
}

void AnimPlayer::sample()
{
    Impl& im = *impl_;
    const ozz::animation::Skeleton& skeleton = im.skeleton->impl().skeleton;

    if (im.layers.empty())
    {
        const auto rest = skeleton.joint_rest_poses();
        std::copy(rest.begin(), rest.end(), im.blended.begin());
    }
    else if (im.layers.size() == 1)
    {
        im.sample_layer(0, im.blended);   // the common case — no blend pass
    }
    else
    {
        if (im.layer_locals.size() < im.layers.size()) im.layer_locals.resize(im.layers.size());
        std::vector<ozz::animation::BlendingJob::Layer> blend_layers(im.layers.size());
        for (std::size_t l = 0; l < im.layers.size(); ++l)
        {
            im.layer_locals[l].resize(skeleton.num_soa_joints());
            im.sample_layer(l, im.layer_locals[l]);
            blend_layers[l].transform = ozz::make_span(im.layer_locals[l]);
            blend_layers[l].weight = im.layers[l].weight;
        }
        ozz::animation::BlendingJob blend;
        blend.layers = { blend_layers.data(), blend_layers.size() };
        blend.rest_pose = skeleton.joint_rest_poses();
        blend.output = ozz::make_span(im.blended);
        if (!blend.Run()) STRING_LOG_WARN("[anim] BlendingJob failed ({} layers)", im.layers.size());
    }

    ozz::animation::LocalToModelJob ltm;
    ltm.skeleton = &skeleton;
    ltm.input = ozz::make_span(im.blended);
    ltm.output = ozz::make_span(im.models);
    if (!ltm.Run()) STRING_LOG_WARN("[anim] LocalToModelJob failed");

    // ozz Float4x4 -> glm::mat4: both column-major with identical 64B stride, so one memcpy
    // over the contiguous array is exact. NOT a cast: Float4x4 is 16-byte-aligned SIMD
    // (__m128 columns) and accessing it as float violates strict aliasing.
    static_assert(sizeof(ozz::math::Float4x4) == sizeof(glm::mat4));
    if (!im.models.empty())
        std::memcpy(im.glm_models.data(), im.models.data(), im.models.size() * sizeof(glm::mat4));
}

std::span<const glm::mat4> AnimPlayer::model_space() const
{
    return impl_->glm_models;
}

// --- CrossFade --------------------------------------------------------------------------------

void CrossFade::play(const Clip* clip, float fade_seconds)
{
    if (!layers_.empty() && layers_.front().clip == clip) return;   // already current
    if (layers_.empty() || fade_seconds <= 0.0f)
    {
        layers_.assign(1, AnimPlayer::Layer{ clip, 0.0f, 1.0f });
        fade_remaining_ = fade_duration_ = 0.0f;
        return;
    }
    // The current front becomes the outgoing layer, KEEPING its time (the fade blends away
    // from the mid-stride pose, not from the clip's start). A fade already in flight is
    // truncated — its own outgoing layer is dropped; two layers is the whole point of this
    // type, anything richer is game code.
    AnimPlayer::Layer outgoing = layers_.front();
    outgoing.weight = 1.0f;
    layers_.assign(1, AnimPlayer::Layer{ clip, 0.0f, 0.0f });
    layers_.push_back(outgoing);
    fade_remaining_ = fade_duration_ = fade_seconds;
}

void CrossFade::advance(float dt, float rate)
{
    for (AnimPlayer::Layer& layer : layers_)
    {
        if (layer.clip == nullptr) continue;
        const float duration = layer.clip->duration();
        layer.time += dt * rate;
        if (duration > 0.0f) layer.time -= duration * std::floor(layer.time / duration);
    }
    if (layers_.size() < 2) return;
    fade_remaining_ = std::max(0.0f, fade_remaining_ - dt);
    const float in = fade_duration_ > 0.0f ? 1.0f - fade_remaining_ / fade_duration_ : 1.0f;
    layers_[0].weight = in;
    layers_[1].weight = 1.0f - in;
    if (fade_remaining_ <= 0.0f) layers_.resize(1);   // outgoing layer done
}

}  // namespace string::anim
