#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

namespace string::anim
{

// ============================================================================
// string::anim — sampling + blending (brief 23)
// ============================================================================
//
// The engine's animation runtime: deserialize `.anim` packs (skeleton + clips, opaque ozz
// archives — see anim_pack.hpp), sample and N-way-blend clips into model-space joint
// matrices, and build the GPU joint palettes. SAMPLING + BLENDING ONLY — the spec locks
// state machines out of the engine; CrossFade below is deliberately the most a caller gets.
//
// ozz stays behind pimpl (the GltfParsed::Impl idiom): consumers of this header never need
// ozz include dirs. Only string-core's src/anim/ TUs touch ozz types.

// A deserialized ozz skeleton. Shared: every AnimPlayer and every paperdoll part cooked
// against the same skeleton holds the same instance.
class Skeleton
{
public:
    // Deserialize from a pack's skeleton blob. nullptr on a malformed/mismatched archive.
    static std::shared_ptr<Skeleton> from_blob(std::span<const uint8_t> blob);
    ~Skeleton();

    uint32_t joint_count() const;
    // Joint index by name, -1 if absent — the future attachment-socket hook.
    int32_t find_joint(std::string_view name) const;

    struct Impl;
    const Impl& impl() const { return *impl_; }

private:
    Skeleton();
    std::unique_ptr<Impl> impl_;
};

// One animation clip (a deserialized ozz Animation).
class Clip
{
public:
    static std::shared_ptr<Clip> from_blob(std::span<const uint8_t> blob, std::string name);
    ~Clip();

    float duration() const;   // seconds
    const std::string& name() const;

    struct Impl;
    const Impl& impl() const { return *impl_; }

private:
    Clip();
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

// One `.anim` pack: the skeleton + its named clips.
class AnimSet
{
public:
    // Load + deserialize a pack. nullptr (with one WARN) on any failure — the caller
    // degrades to bind pose, never crashes on a stale pack.
    static std::shared_ptr<AnimSet> load(const std::filesystem::path& pack_path);

    const std::shared_ptr<Skeleton>& skeleton() const { return skeleton_; }
    const Clip* find(std::string_view name) const;
    std::span<const std::shared_ptr<Clip>> clips() const { return clips_; }
    // Must equal CookedSkin::skeleton_hash for the mesh parts this pack animates.
    uint64_t skeleton_hash() const { return skeleton_hash_; }

private:
    std::shared_ptr<Skeleton> skeleton_;
    std::vector<std::shared_ptr<Clip>> clips_;
    uint64_t skeleton_hash_ = 0;
};

// One playing instance: N weighted layers -> sampled local poses -> blend -> model-space
// matrices. Buffers (per-layer sampling contexts, SoA locals) are owned here and reused
// across frames; a single layer skips the blend entirely.
class AnimPlayer
{
public:
    explicit AnimPlayer(std::shared_ptr<Skeleton> skeleton);
    ~AnimPlayer();
    AnimPlayer(AnimPlayer&&) noexcept;
    AnimPlayer& operator=(AnimPlayer&&) noexcept;

    struct Layer
    {
        const Clip* clip = nullptr;
        float time = 0.0f;     // seconds; clamped into the clip by sampling
        float weight = 1.0f;   // blend weight; ozz normalizes across layers
    };

    void set_layers(std::span<const Layer> layers);
    // SamplingJob per layer -> BlendingJob (skipped for one layer) -> LocalToModelJob.
    // No layers => the skeleton's rest pose.
    void sample();
    // Model-space joint matrices in OZZ joint order (build_palette applies the remap).
    std::span<const glm::mat4> model_space() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A two-layer cross-fade driver for AnimPlayer. Deliberately trivial — anything richer
// (transition rules, states, conditions) is a state machine, which is game code.
class CrossFade
{
public:
    // Fade to `clip` over `fade_seconds` (0 = snap). Fading to the current clip is a no-op.
    void play(const Clip* clip, float fade_seconds);
    // Advance both layers' times (looping: time wraps at each clip's duration) and the fade.
    void advance(float dt, float rate = 1.0f);
    std::span<const AnimPlayer::Layer> layers() const { return layers_; }

private:
    std::vector<AnimPlayer::Layer> layers_;   // [incoming, outgoing?]
    float fade_remaining_ = 0.0f;
    float fade_duration_ = 0.0f;
};

// palette[i] = model_space[remap[i]] * inverse_bind[i], i in glTF joint order. `out` is the
// draw's palette window (out.size() == remap.size() == inverse_bind.size()).
void build_palette(std::span<const glm::mat4> model_space, std::span<const uint32_t> remap,
                   std::span<const glm::mat4> inverse_bind, std::span<glm::mat4> out);

}  // namespace string::anim
