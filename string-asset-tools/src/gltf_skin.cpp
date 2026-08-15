#include <string/asset/tools/gltf_skin.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>

#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/endianness.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

// The `.anim` blobs' wire format is ozz's per-type archive version, so a bump there silently
// invalidates every pack. Same tripwire discipline as bake.cpp's MESHOPTIMIZER_VERSION assert:
// make the divergence a compile error that names the fix (bump kAnimPackVersion -> re-cook).
static_assert(ozz::io::internal::Version<const ozz::animation::Animation>::kValue == 7,
              "ozz Animation archive version changed: bump kAnimPackVersion and re-cook");
static_assert(ozz::io::internal::Version<const ozz::animation::Skeleton>::kValue == 2,
              "ozz Skeleton archive version changed: bump kAnimPackVersion and re-cook");

#include <glm/gtc/type_ptr.hpp>

#include <string/anim/anim_pack.hpp>
#include <string/core/logger.hpp>

#include "gltf_impl.hpp"

namespace string::asset::tools
{
namespace
{

// STEP and CUBICSPLINE glTF samplers are resampled to linear keys at this rate (ozz
// RawAnimation keys are linear-only).
constexpr float kResampleHz = 30.0f;

// Poses sampled per clip for the animated bound: ratios k/16 for k in 0..16 — fixed count
// and order, so the bound is deterministic.
constexpr uint32_t kAnimBoundSamples = 17;

// A skin with clips gets the sampled-pose union; a skin WITHOUT clips falls back to its
// bind-pose bound scaled by this (the pose is unknown, be conservative).
constexpr float kBindPoseBoundScale = 1.25f;

// FNV-1a (same scheme as bake.cpp's content hash — small enough to duplicate rather than
// export from the geometry core).
uint64_t fnv1a(const void* data, size_t len, uint64_t seed = 1469598103934665603ULL)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) { h ^= bytes[i]; h *= 1099511628211ULL; }
    return h;
}

uint64_t align16(uint64_t n) { return (n + 15u) & ~uint64_t(15u); }

// Serialize one ozz runtime object through an OArchive. Endianness is FORCED little so a
// cook is byte-identical regardless of host (the pack never leaves x86 today, but the
// determinism gate should not depend on that staying true).
template <typename T>
std::vector<uint8_t> serialize_ozz(const T& object)
{
    ozz::io::MemoryStream stream;
    ozz::io::OArchive archive(&stream, ozz::kLittleEndian);
    archive << object;
    std::vector<uint8_t> bytes(stream.Size());
    stream.Seek(0, ozz::io::Stream::kSet);
    stream.Read(bytes.data(), bytes.size());
    return bytes;
}

// A node's local TRS (matrix nodes are decomposed — glTF allows either form).
ozz::math::Transform node_rest_pose(const fastgltf::Node& node)
{
    fastgltf::TRS trs;
    if (const auto* direct = std::get_if<fastgltf::TRS>(&node.transform))
    {
        trs = *direct;
    }
    else
    {
        fastgltf::math::decomposeTransformMatrix(std::get<fastgltf::math::fmat4x4>(node.transform),
                                                 trs.scale, trs.rotation, trs.translation);
    }
    ozz::math::Transform out;
    out.translation = { trs.translation.x(), trs.translation.y(), trs.translation.z() };
    out.rotation = { trs.rotation.x(), trs.rotation.y(), trs.rotation.z(), trs.rotation.w() };
    out.scale = { trs.scale.x(), trs.scale.y(), trs.scale.z() };
    return out;
}

// The skeleton's node set: every joint of every skin, closed over ALL ancestors up to the
// scene root. Ancestors matter because glTF joint matrices are GLOBAL node transforms — an
// armature node above the root joint contributes its transform even though it is not in any
// skin's joints array; dropping it would misplace the whole character.
std::vector<std::size_t> skeleton_node_closure(const fastgltf::Asset& asset)
{
    std::vector<int64_t> parent(asset.nodes.size(), -1);
    for (std::size_t n = 0; n < asset.nodes.size(); ++n)
        for (const std::size_t child : asset.nodes[n].children)
            parent[child] = static_cast<int64_t>(n);

    std::vector<bool> in_set(asset.nodes.size(), false);
    for (const fastgltf::Skin& skin : asset.skins)
    {
        for (const std::size_t joint : skin.joints)
        {
            for (int64_t n = static_cast<int64_t>(joint); n >= 0 && !in_set[n]; n = parent[n])
                in_set[n] = true;
        }
    }

    std::vector<std::size_t> closure;   // ascending node index — deterministic
    for (std::size_t n = 0; n < asset.nodes.size(); ++n)
        if (in_set[n]) closure.push_back(n);
    return closure;
}

// Unique, deterministic joint names: the node's own name when unique, disambiguated with the
// node index otherwise (ozz joint lookup is by name, and the name is how node indices map to
// ozz joint indices after the builder reorders depth-first).
std::vector<std::string> joint_names_for(const fastgltf::Asset& asset,
                                         const std::vector<std::size_t>& closure)
{
    std::vector<std::string> names(asset.nodes.size());
    std::map<std::string, uint32_t> used;
    for (const std::size_t n : closure)
    {
        std::string name(asset.nodes[n].name.begin(), asset.nodes[n].name.end());
        if (name.empty()) name = "node" + std::to_string(n);
        if (++used[name] > 1) name += "#" + std::to_string(n);
        names[n] = std::move(name);
    }
    return names;
}

// Build the ozz skeleton over the closure. Children recurse in ascending node-index order
// (the closure's order), so the raw hierarchy — and therefore the built skeleton — is
// deterministic. Returns the node -> ozz joint map alongside the skeleton.
struct BuiltSkeleton
{
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    std::vector<int32_t> node_to_ozz;   // -1 = node not in the skeleton
};

BuiltSkeleton build_skeleton(const fastgltf::Asset& asset,
                             const std::vector<std::size_t>& closure,
                             const std::vector<std::string>& names)
{
    std::vector<bool> in_set(asset.nodes.size(), false);
    for (const std::size_t n : closure) in_set[n] = true;

    std::function<void(std::size_t, ozz::animation::offline::RawSkeleton::Joint&)> fill =
        [&](std::size_t node_index, ozz::animation::offline::RawSkeleton::Joint& joint) {
            joint.name = names[node_index].c_str();
            joint.transform = node_rest_pose(asset.nodes[node_index]);
            for (const std::size_t child : asset.nodes[node_index].children)
            {
                if (!in_set[child]) continue;
                joint.children.emplace_back();
                fill(child, joint.children.back());
            }
        };

    std::vector<int64_t> parent(asset.nodes.size(), -1);
    for (std::size_t n = 0; n < asset.nodes.size(); ++n)
        for (const std::size_t child : asset.nodes[n].children)
            parent[child] = static_cast<int64_t>(n);

    ozz::animation::offline::RawSkeleton raw;
    for (const std::size_t n : closure)
    {
        if (parent[n] >= 0 && in_set[parent[n]]) continue;   // not a root
        raw.roots.emplace_back();
        fill(n, raw.roots.back());
    }
    if (!raw.Validate()) throw std::runtime_error("skin cook: skeleton hierarchy failed validation");

    ozz::animation::offline::SkeletonBuilder builder;
    BuiltSkeleton out;
    out.skeleton = builder(raw);
    if (!out.skeleton) throw std::runtime_error("skin cook: ozz SkeletonBuilder rejected the skeleton");

    // Names are unique by construction, so name -> ozz index inverts to node -> ozz index.
    std::map<std::string, int32_t> by_name;
    const auto ozz_names = out.skeleton->joint_names();
    for (int32_t j = 0; j < static_cast<int32_t>(ozz_names.size()); ++j)
        by_name[ozz_names[j]] = j;
    out.node_to_ozz.assign(asset.nodes.size(), -1);
    for (const std::size_t n : closure) out.node_to_ozz[n] = by_name.at(names[n]);
    return out;
}

// --- Clip extraction ---------------------------------------------------------------------

// One channel's keys, read out of its sampler and (for STEP/CUBICSPLINE) resampled to the
// linear keys ozz supports. VecT is glm::vec3 (translation/scale) or glm::vec4 (rotation).
template <typename VecT>
struct ChannelKeys
{
    std::vector<float> times;
    std::vector<VecT> values;
};

template <typename VecT>
VecT hermite(const VecT& v0, const VecT& out_tangent0, const VecT& v1, const VecT& in_tangent1,
             float dt, float u)
{
    const float u2 = u * u, u3 = u2 * u;
    return v0 * (2.0f * u3 - 3.0f * u2 + 1.0f) + out_tangent0 * (dt * (u3 - 2.0f * u2 + u)) +
           v1 * (-2.0f * u3 + 3.0f * u2) + in_tangent1 * (dt * (u3 - u2));
}

// Evaluate a sampler at time t. `values` holds 1 element per key (LINEAR/STEP) or 3 per key
// (CUBICSPLINE: in-tangent, value, out-tangent). Times are sorted (glTF requires it).
template <typename VecT>
VecT sample_channel(const std::vector<float>& times, const std::vector<VecT>& values,
                    fastgltf::AnimationInterpolation mode, float t)
{
    const auto stride = mode == fastgltf::AnimationInterpolation::CubicSpline ? 3u : 1u;
    const auto value_of = [&](std::size_t k) { return values[k * stride + (stride == 3u ? 1u : 0u)]; };
    if (t <= times.front()) return value_of(0);
    if (t >= times.back()) return value_of(times.size() - 1);
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    const std::size_t k1 = static_cast<std::size_t>(it - times.begin());
    const std::size_t k0 = k1 - 1;
    if (mode == fastgltf::AnimationInterpolation::Step) return value_of(k0);
    const float dt = times[k1] - times[k0];
    const float u = dt > 0.0f ? (t - times[k0]) / dt : 0.0f;
    if (mode == fastgltf::AnimationInterpolation::CubicSpline)
    {
        return hermite(value_of(k0), values[k0 * 3 + 2], value_of(k1), values[k1 * 3 + 0], dt, u);
    }
    return value_of(k0) * (1.0f - u) + value_of(k1) * u;   // Linear
}

// Read a channel's sampler into linear (time, value) keys: LINEAR passes through untouched,
// STEP/CUBICSPLINE are resampled at kResampleHz across [0, duration].
template <typename VecT>
ChannelKeys<VecT> read_channel(const fastgltf::Asset& asset, const fastgltf::AnimationSampler& sampler,
                               float duration)
{
    std::vector<float> times;
    fastgltf::iterateAccessor<float>(asset, asset.accessors[sampler.inputAccessor],
                                     [&](float t) { times.push_back(t); });
    std::vector<VecT> values;
    fastgltf::iterateAccessor<VecT>(asset, asset.accessors[sampler.outputAccessor],
                                    [&](VecT v) { values.push_back(v); });
    ChannelKeys<VecT> out;
    if (times.empty() || values.empty()) return out;

    if (sampler.interpolation == fastgltf::AnimationInterpolation::Linear)
    {
        out.times = std::move(times);
        out.values = std::move(values);
        return out;
    }
    const uint32_t steps = std::max(1u, static_cast<uint32_t>(std::ceil(duration * kResampleHz)));
    out.times.reserve(steps + 1);
    out.values.reserve(steps + 1);
    for (uint32_t k = 0; k <= steps; ++k)
    {
        const float t = std::min(duration, static_cast<float>(k) / kResampleHz);
        out.times.push_back(t);
        out.values.push_back(sample_channel(times, values, sampler.interpolation, t));
        if (t >= duration) break;
    }
    return out;
}

float animation_duration(const fastgltf::Asset& asset, const fastgltf::Animation& animation)
{
    float duration = 0.0f;
    for (const fastgltf::AnimationSampler& sampler : animation.samplers)
    {
        fastgltf::iterateAccessor<float>(asset, asset.accessors[sampler.inputAccessor],
                                         [&](float t) { duration = std::max(duration, t); });
    }
    // ozz requires duration > 0; a single-pose "clip" still needs a valid timeline.
    return std::max(duration, 1.0f / kResampleHz);
}

// Build one ozz RawAnimation from a glTF animation. Every skeleton joint starts with a
// single rest-pose key (an EMPTY ozz track means identity, which collapses un-animated
// joints); channels then replace their component's keys. Returns the number of channels
// dropped because they target non-skeleton nodes (props) or morph weights.
uint32_t fill_raw_animation(const fastgltf::Asset& asset, const fastgltf::Animation& animation,
                            const ozz::animation::Skeleton& skeleton,
                            const std::vector<int32_t>& node_to_ozz, const std::string& clip_name,
                            ozz::animation::offline::RawAnimation& raw)
{
    raw.name = clip_name.c_str();
    raw.duration = animation_duration(asset, animation);
    raw.tracks.resize(skeleton.num_joints());
    // Rest poses are SoA (4 joints per element, one SimdFloat4 per component lane) — store
    // each lane register and pick this joint's lane out.
    const auto rest_poses = skeleton.joint_rest_poses();
    const auto lane = [](ozz::math::SimdFloat4 v, int l) {
        float f[4];
        ozz::math::StorePtrU(v, f);
        return f[l];
    };
    for (int j = 0; j < skeleton.num_joints(); ++j)
    {
        const ozz::math::SoaTransform& soa = rest_poses[j / 4];
        const int l = j % 4;
        raw.tracks[j].translations = { { 0.0f,
            { lane(soa.translation.x, l), lane(soa.translation.y, l), lane(soa.translation.z, l) } } };
        raw.tracks[j].rotations = { { 0.0f,
            { lane(soa.rotation.x, l), lane(soa.rotation.y, l), lane(soa.rotation.z, l),
              lane(soa.rotation.w, l) } } };
        raw.tracks[j].scales = { { 0.0f,
            { lane(soa.scale.x, l), lane(soa.scale.y, l), lane(soa.scale.z, l) } } };
    }

    uint32_t dropped = 0;
    for (const fastgltf::AnimationChannel& channel : animation.channels)
    {
        const int32_t joint = channel.nodeIndex.has_value() &&
                              *channel.nodeIndex < node_to_ozz.size()
                                  ? node_to_ozz[*channel.nodeIndex]
                                  : -1;
        if (joint < 0 || channel.path == fastgltf::AnimationPath::Weights)
        {
            ++dropped;   // prop-node channel or morph weights — not skeletal data
            continue;
        }
        const fastgltf::AnimationSampler& sampler = animation.samplers[channel.samplerIndex];
        auto& track = raw.tracks[joint];
        if (channel.path == fastgltf::AnimationPath::Translation)
        {
            const auto keys = read_channel<glm::vec3>(asset, sampler, raw.duration);
            track.translations.clear();
            for (std::size_t k = 0; k < keys.times.size(); ++k)
                track.translations.push_back(
                    { keys.times[k], { keys.values[k].x, keys.values[k].y, keys.values[k].z } });
        }
        else if (channel.path == fastgltf::AnimationPath::Scale)
        {
            const auto keys = read_channel<glm::vec3>(asset, sampler, raw.duration);
            track.scales.clear();
            for (std::size_t k = 0; k < keys.times.size(); ++k)
                track.scales.push_back(
                    { keys.times[k], { keys.values[k].x, keys.values[k].y, keys.values[k].z } });
        }
        else   // Rotation
        {
            const auto keys = read_channel<glm::vec4>(asset, sampler, raw.duration);
            track.rotations.clear();
            for (std::size_t k = 0; k < keys.times.size(); ++k)
            {
                const glm::vec4 q = keys.values[k];
                const float len2 = glm::dot(q, q);
                // ozz's NormalizeSafe would SILENTLY substitute identity for a degenerate
                // quaternion — a wrong animation with no error. Reject it at the cook instead.
                if (len2 < 1e-6f)
                    throw std::runtime_error("skin cook: degenerate rotation key in clip '" +
                                             clip_name + "'");
                const glm::vec4 n = q * (1.0f / std::sqrt(len2));
                track.rotations.push_back({ keys.times[k], { n.x, n.y, n.z, n.w } });
            }
        }
    }
    return dropped;
}

// --- Animated bounds ---------------------------------------------------------------------

// Per-joint influence radius in bind space: how far from joint i any vertex it influences
// sits, measured where the palette matrix maps FROM (ibm * position). In any pose, joint i's
// influence then lies inside the sphere (translation(M_i), r_i * max_axis_scale(M_i)).
std::vector<float> influence_radii(const GltfGeometry& geometry, int32_t skin_index,
                                   std::span<const glm::mat4> ibms)
{
    std::vector<float> radii(ibms.size(), 0.0f);
    for (const GltfDraw& draw : geometry.draws)
    {
        if (draw.skin != skin_index) continue;
        for (uint32_t k = 0; k < draw.index_count; ++k)
        {
            const uint32_t v = geometry.indices[draw.index_offset + k];
            for (int inf = 0; inf < 4; ++inf)
            {
                if (geometry.weights[v][inf] <= 0.0f) continue;
                const uint32_t j = geometry.joints[v][inf];
                if (j >= radii.size()) continue;   // validated separately; stay in bounds here
                const glm::vec3 p = glm::vec3(ibms[j] * glm::vec4(geometry.vertices[v].pos, 1.0f));
                radii[j] = std::max(radii[j], glm::length(p));
            }
        }
    }
    return radii;
}

float max_axis_scale(const glm::mat4& m)
{
    const float x = glm::dot(glm::vec3(m[0]), glm::vec3(m[0]));
    const float y = glm::dot(glm::vec3(m[1]), glm::vec3(m[1]));
    const float z = glm::dot(glm::vec3(m[2]), glm::vec3(m[2]));
    return std::sqrt(std::max(x, std::max(y, z)));
}

// Union the joint influence spheres of one posed skeleton into the AABB.
void union_pose(std::span<const glm::mat4> models, std::span<const uint32_t> remap,
                std::span<const float> radii, glm::vec3& out_min, glm::vec3& out_max)
{
    for (std::size_t i = 0; i < remap.size(); ++i)
    {
        if (radii[i] <= 0.0f) continue;   // joint influences nothing
        const glm::mat4& m = models[remap[i]];
        const glm::vec3 center = glm::vec3(m[3]);
        const float radius = radii[i] * max_axis_scale(m);
        out_min = glm::min(out_min, center - glm::vec3(radius));
        out_max = glm::max(out_max, center + glm::vec3(radius));
    }
}

// Model-space matrices for one sampled pose (or the rest pose when `animation` is null).
std::vector<glm::mat4> pose_models(const ozz::animation::Skeleton& skeleton,
                                   const ozz::animation::Animation* animation,
                                   ozz::animation::SamplingJob::Context& context, float ratio)
{
    ozz::vector<ozz::math::SoaTransform> locals(skeleton.num_soa_joints());
    if (animation != nullptr)
    {
        ozz::animation::SamplingJob sampling;
        sampling.animation = animation;
        sampling.context = &context;
        sampling.ratio = ratio;
        sampling.output = ozz::make_span(locals);
        if (!sampling.Run()) throw std::runtime_error("skin cook: SamplingJob failed");
    }
    else
    {
        const auto rest = skeleton.joint_rest_poses();
        std::copy(rest.begin(), rest.end(), locals.begin());
    }
    ozz::vector<ozz::math::Float4x4> models(skeleton.num_joints());
    ozz::animation::LocalToModelJob ltm;
    ltm.skeleton = &skeleton;
    ltm.input = ozz::make_span(locals);
    ltm.output = ozz::make_span(models);
    if (!ltm.Run()) throw std::runtime_error("skin cook: LocalToModelJob failed");

    std::vector<glm::mat4> out(models.size());
    for (std::size_t m = 0; m < models.size(); ++m)
        std::memcpy(&out[m], &models[m], sizeof(glm::mat4));   // both column-major; Float4x4 is
                                                               // 16B-aligned SIMD, mat4 is not —
                                                               // memcpy, never a cast
    return out;
}

}  // namespace

SkinVertex quantize_skin_vertex(const glm::u16vec4& joints, const glm::vec4& weights)
{
    // Influence list, sorted by descending weight (ascending joint index breaks ties), top 4.
    struct Influence { uint16_t joint; float weight; };
    Influence inf[4];
    for (int i = 0; i < 4; ++i) inf[i] = { joints[i], std::max(weights[i], 0.0f) };
    std::sort(std::begin(inf), std::end(inf), [](const Influence& a, const Influence& b) {
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.joint < b.joint;
    });

    const float sum = inf[0].weight + inf[1].weight + inf[2].weight + inf[3].weight;
    SkinVertex out{};
    if (sum <= 0.0f)
    {
        out.weights[0] = 255;   // degenerate -> canonical: all of joint 0
        return out;
    }

    // floor(w*255), then hand the remainder out one unit at a time to the largest fractional
    // parts (ties to the LOWER influence index) so the four always sum to exactly 255.
    uint32_t quantized[4];
    float fraction[4];
    uint32_t total = 0;
    for (int i = 0; i < 4; ++i)
    {
        const float scaled = inf[i].weight / sum * 255.0f;
        quantized[i] = static_cast<uint32_t>(scaled);
        fraction[i] = scaled - static_cast<float>(quantized[i]);
        total += quantized[i];
    }
    for (uint32_t remainder = 255 - total; remainder > 0; --remainder)
    {
        int best = 0;
        for (int i = 1; i < 4; ++i)
            if (fraction[i] > fraction[best]) best = i;
        ++quantized[best];
        fraction[best] = -1.0f;
    }
    for (int i = 0; i < 4; ++i)
    {
        // A zero-weight influence writes the canonical joint 0 / weight 0 padding.
        out.joints[i] = quantized[i] > 0 ? static_cast<uint8_t>(inf[i].joint) : 0;
        out.weights[i] = static_cast<uint8_t>(quantized[i]);
    }
    return out;
}

SkinCook cook_skins(const GltfParsed& parsed, const GltfGeometry& geometry)
{
    SkinCook out;
    const fastgltf::Asset& asset = parsed.impl->asset;
    if (asset.skins.empty()) return out;

    // --- One skeleton covering every skin (the shared-skeleton paperdoll shape) -----------
    const std::vector<std::size_t> closure = skeleton_node_closure(asset);
    const std::vector<std::string> names = joint_names_for(asset, closure);
    const BuiltSkeleton built = build_skeleton(asset, closure, names);
    const ozz::animation::Skeleton& skeleton = *built.skeleton;

    // --- Clips ------------------------------------------------------------------------------
    ozz::animation::offline::AnimationBuilder animation_builder;
    std::vector<ozz::unique_ptr<ozz::animation::Animation>> clips;
    std::vector<std::string> clip_names;
    for (std::size_t a = 0; a < asset.animations.size(); ++a)
    {
        const fastgltf::Animation& animation = asset.animations[a];
        std::string name(animation.name.begin(), animation.name.end());
        if (name.empty()) name = "clip" + std::to_string(a);

        ozz::animation::offline::RawAnimation raw;
        const uint32_t dropped =
            fill_raw_animation(asset, animation, skeleton, built.node_to_ozz, name, raw);
        if (dropped > 0)
            STRING_LOG_INFO("[cook] clip '{}': {} non-skeletal channel(s) dropped", name, dropped);

        ozz::unique_ptr<ozz::animation::Animation> clip = animation_builder(raw);
        if (!clip)
            throw std::runtime_error("skin cook: ozz AnimationBuilder rejected clip '" + name + "'");
        clips.push_back(std::move(clip));
        clip_names.push_back(std::move(name));
    }
    out.clip_count = static_cast<uint32_t>(clips.size());

    // --- Per-skin tables: IBMs, remap, animated bounds --------------------------------------
    for (std::size_t s = 0; s < asset.skins.size(); ++s)
    {
        const fastgltf::Skin& skin = asset.skins[s];
        std::string skin_name(skin.name.begin(), skin.name.end());
        if (skin.joints.size() > 256)
            throw std::runtime_error("skin cook: skin '" + skin_name + "' has " +
                                     std::to_string(skin.joints.size()) +
                                     " joints; the SkinVertex format caps a palette at 256");

        CookedSkin cooked{};
        cooked.joint_count = static_cast<uint32_t>(skin.joints.size());
        cooked.ibm_offset = static_cast<uint32_t>(out.inverse_bind.size());
        cooked.remap_offset = static_cast<uint32_t>(out.joint_remap.size());

        if (skin.inverseBindMatrices.has_value())
        {
            fastgltf::iterateAccessor<glm::mat4>(
                asset, asset.accessors[*skin.inverseBindMatrices],
                [&](glm::mat4 m) { out.inverse_bind.push_back(m); });
        }
        else
        {
            out.inverse_bind.resize(out.inverse_bind.size() + skin.joints.size(), glm::mat4(1.0f));
        }
        for (const std::size_t joint : skin.joints)
            out.joint_remap.push_back(static_cast<uint32_t>(built.node_to_ozz[joint]));

        // Animated bound: union of every joint sphere over every sampled pose of every clip,
        // plus the bind pose (the hash-mismatch fallback renders it). No clips at all ->
        // bind-pose bound, conservatively scaled.
        const std::span<const glm::mat4> ibms{ out.inverse_bind.data() + cooked.ibm_offset,
                                               cooked.joint_count };
        const std::span<const uint32_t> remap{ out.joint_remap.data() + cooked.remap_offset,
                                               cooked.joint_count };
        const std::vector<float> radii =
            influence_radii(geometry, static_cast<int32_t>(s), ibms);

        glm::vec3 bound_min(std::numeric_limits<float>::max());
        glm::vec3 bound_max(std::numeric_limits<float>::lowest());
        ozz::animation::SamplingJob::Context context(skeleton.num_joints());
        union_pose(pose_models(skeleton, nullptr, context, 0.0f), remap, radii, bound_min, bound_max);
        if (clips.empty())
        {
            const glm::vec3 center = (bound_min + bound_max) * 0.5f;
            bound_min = center + (bound_min - center) * kBindPoseBoundScale;
            bound_max = center + (bound_max - center) * kBindPoseBoundScale;
        }
        for (const auto& clip : clips)
        {
            context.Invalidate();
            for (uint32_t k = 0; k < kAnimBoundSamples; ++k)
            {
                const float ratio = static_cast<float>(k) / static_cast<float>(kAnimBoundSamples - 1);
                union_pose(pose_models(skeleton, clip.get(), context, ratio), remap, radii,
                           bound_min, bound_max);
            }
        }
        out.skin_anim_min.push_back(bound_min);
        out.skin_anim_max.push_back(bound_max);
        out.skins.push_back(cooked);
        out.names.push_back(std::move(skin_name));   // v5
    }

    // --- Quantize the vertex stream ----------------------------------------------------------
    out.skin_vertices.resize(geometry.vertices.size());
    for (std::size_t v = 0; v < geometry.vertices.size(); ++v)
        out.skin_vertices[v] = quantize_skin_vertex(geometry.joints[v], geometry.weights[v]);

    // --- Serialize the pack ------------------------------------------------------------------
    const std::vector<uint8_t> skeleton_blob = serialize_ozz(skeleton);
    const uint64_t skeleton_hash = fnv1a(skeleton_blob.data(), skeleton_blob.size());
    for (CookedSkin& cooked : out.skins) cooked.skeleton_hash = skeleton_hash;

    std::vector<std::vector<uint8_t>> clip_blobs;
    clip_blobs.reserve(clips.size());
    for (const auto& clip : clips) clip_blobs.push_back(serialize_ozz(*clip));

    anim::AnimPackHeader header{};
    std::memcpy(header.magic, anim::kAnimPackMagic, sizeof(header.magic));
    header.version = anim::kAnimPackVersion;
    header.joint_count = static_cast<uint32_t>(skeleton.num_joints());
    header.clip_count = out.clip_count;
    header.skeleton_hash = skeleton_hash;
    uint64_t cursor = align16(sizeof(anim::AnimPackHeader));
    header.clips_offset = cursor;
    cursor += align16(uint64_t(out.clip_count) * sizeof(anim::AnimClipDesc));
    header.skeleton_offset = cursor;
    header.skeleton_bytes = skeleton_blob.size();
    cursor += align16(skeleton_blob.size());

    std::vector<anim::AnimClipDesc> descs(out.clip_count);
    for (std::size_t c = 0; c < clips.size(); ++c)
    {
        anim::AnimClipDesc& d = descs[c];
        if (clip_names[c].size() >= sizeof(d.name))
            STRING_LOG_WARN("[cook] clip name '{}' truncated to {} chars", clip_names[c],
                            sizeof(d.name) - 1);
        std::memcpy(d.name, clip_names[c].data(),
                    std::min(clip_names[c].size(), sizeof(d.name) - 1));
        d.blob_offset = cursor;
        d.blob_bytes = clip_blobs[c].size();
        d.duration = clips[c]->duration();
        d.track_count = static_cast<uint32_t>(clips[c]->num_tracks());
        cursor += align16(clip_blobs[c].size());
    }

    out.anim_pack.assign(cursor, 0);   // zero-filled => padding is deterministic
    std::memcpy(out.anim_pack.data(), &header, sizeof(header));
    if (!descs.empty())
        std::memcpy(out.anim_pack.data() + header.clips_offset, descs.data(),
                    descs.size() * sizeof(anim::AnimClipDesc));
    std::memcpy(out.anim_pack.data() + header.skeleton_offset, skeleton_blob.data(),
                skeleton_blob.size());
    for (std::size_t c = 0; c < clip_blobs.size(); ++c)
        std::memcpy(out.anim_pack.data() + descs[c].blob_offset, clip_blobs[c].data(),
                    clip_blobs[c].size());

    STRING_LOG_INFO("[cook] skins: {} skin(s), {} skeleton joints, {} clip(s), pack {} bytes",
                    out.skins.size(), skeleton.num_joints(), out.clip_count,
                    out.anim_pack.size());
    return out;
}

}  // namespace string::asset::tools
