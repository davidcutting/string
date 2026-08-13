#include <string/anim/anim.hpp>

#include <cstring>

#include <string/anim/anim_pack.hpp>
#include <string/core/logger.hpp>

#include "ozz_impl.hpp"

namespace string::anim
{

// --- Skeleton ---------------------------------------------------------------------------------

Skeleton::Skeleton() : impl_(std::make_unique<Impl>()) {}
Skeleton::~Skeleton() = default;

std::shared_ptr<Skeleton> Skeleton::from_blob(std::span<const uint8_t> blob)
{
    std::shared_ptr<Skeleton> out(new Skeleton());
    if (!deserialize_ozz(blob, out->impl_->skeleton)) return nullptr;
    return out;
}

uint32_t Skeleton::joint_count() const
{
    return static_cast<uint32_t>(impl_->skeleton.num_joints());
}

int32_t Skeleton::find_joint(std::string_view name) const
{
    const auto names = impl_->skeleton.joint_names();
    for (int32_t j = 0; j < static_cast<int32_t>(names.size()); ++j)
        if (name == names[j]) return j;
    return -1;
}

// --- Clip -------------------------------------------------------------------------------------

Clip::Clip() : impl_(std::make_unique<Impl>()) {}
Clip::~Clip() = default;

std::shared_ptr<Clip> Clip::from_blob(std::span<const uint8_t> blob, std::string name)
{
    std::shared_ptr<Clip> out(new Clip());
    if (!deserialize_ozz(blob, out->impl_->animation)) return nullptr;
    out->name_ = std::move(name);
    return out;
}

float Clip::duration() const { return impl_->animation.duration(); }
const std::string& Clip::name() const { return name_; }

// --- AnimSet ----------------------------------------------------------------------------------

std::shared_ptr<AnimSet> AnimSet::load(const std::filesystem::path& pack_path)
{
    AnimPack pack;
    if (!read_anim_pack_file(pack_path, pack))
    {
        STRING_LOG_WARN("[anim] cannot read pack {} (missing/stale — re-cook the source)",
                        pack_path.string());
        return nullptr;
    }
    auto set = std::make_shared<AnimSet>();
    set->skeleton_hash_ = pack.header.skeleton_hash;
    set->skeleton_ = Skeleton::from_blob(pack.skeleton());
    if (!set->skeleton_)
    {
        STRING_LOG_WARN("[anim] pack {}: skeleton blob rejected (ozz archive mismatch)",
                        pack_path.string());
        return nullptr;
    }
    for (const AnimClipDesc& desc : pack.clips())
    {
        std::shared_ptr<Clip> clip = Clip::from_blob(pack.clip_blob(desc), desc.name);
        if (!clip)
        {
            STRING_LOG_WARN("[anim] pack {}: clip '{}' rejected, skipped", pack_path.string(),
                            desc.name);
            continue;
        }
        set->clips_.push_back(std::move(clip));
    }
    return set;
}

const Clip* AnimSet::find(std::string_view name) const
{
    for (const auto& clip : clips_)
        if (clip->name() == name) return clip.get();
    return nullptr;
}

// --- Palette ----------------------------------------------------------------------------------

void build_palette(std::span<const glm::mat4> model_space, std::span<const uint32_t> remap,
                   std::span<const glm::mat4> inverse_bind, std::span<glm::mat4> out)
{
    for (std::size_t i = 0; i < remap.size(); ++i)
        out[i] = model_space[remap[i]] * inverse_bind[i];
}

}  // namespace string::anim
