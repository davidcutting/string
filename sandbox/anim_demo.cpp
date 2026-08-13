#include "anim_demo.hpp"

#include <string/anim/anim.hpp>
#include <string/core/logger.hpp>

#include <string/render/geometry_pass.hpp>

#include "debug_cvars.hpp"

namespace sandbox
{

using ::string::anim::AnimPlayer;
using ::string::anim::AnimSet;
using ::string::anim::Clip;
using ::string::anim::CrossFade;
using ::string::render::GeometryScene;

namespace
{
constexpr float kDemoSwitchSeconds = 3.0f;   // auto demo: idle <-> walk cadence
}

struct anim_demo::Character
{
    std::shared_ptr<AnimSet> set;
    AnimPlayer player;
    CrossFade fade;
    GeometryScene::SkinInstance meta;

    Character(std::shared_ptr<AnimSet> s, const GeometryScene::SkinInstance& m)
    : set(std::move(s)), player(set->skeleton()), meta(m)
    {
    }

    // Idle_Loop when the pack has it (the UAL library), else the first clip.
    const Clip* default_clip() const
    {
        if (const Clip* idle = set->find("Idle_Loop")) return idle;
        return set->clips().empty() ? nullptr : set->clips().front().get();
    }
};

anim_demo::anim_demo(const ::string::render::geometry_pass& geo) : geo_(&geo)
{
    for (const GeometryScene::SkinInstance& skin : geo.skins())
    {
        std::shared_ptr<AnimSet> set = AnimSet::load(skin.anim_pack);
        if (!set) continue;   // load already WARNed; bind pose via the ring's identity default
        if (set->skeleton_hash() != skin.skeleton_hash)
        {
            STRING_LOG_WARN("[anim] {}: skeleton hash mismatch (stale pack?) — bind pose",
                            skin.anim_pack.string());
            continue;
        }
        auto character = std::make_unique<Character>(std::move(set), skin);
        if (const Clip* first = character->default_clip()) character->fade.play(first, 0.0f);
        characters_.push_back(std::move(character));
    }
    if (!characters_.empty())
        STRING_LOG_INFO("[anim] driving {} skinned character(s); anim.clip / anim.blend / "
                        "anim.rate / anim.demo control playback",
                        characters_.size());
}

anim_demo::~anim_demo() = default;

void anim_demo::apply_clip_request(const std::string& request, float fade_seconds)
{
    for (const std::unique_ptr<Character>& c : characters_)
    {
        if (const Clip* clip = c->set->find(request))
        {
            c->fade.play(clip, fade_seconds);
            continue;
        }
        std::string names;
        for (const auto& clip : c->set->clips())
            names += (names.empty() ? "" : ", ") + clip->name();
        STRING_LOG_WARN("[anim] no clip '{}' in {}; available: {}", request,
                        c->meta.anim_pack.filename().string(), names);
    }
}

void anim_demo::advance_auto_demo(float dt, float fade_seconds)
{
    demo_timer_ += dt;
    if (demo_timer_ < kDemoSwitchSeconds) return;
    demo_timer_ = 0.0f;
    demo_walking_ = !demo_walking_;
    for (const std::unique_ptr<Character>& c : characters_)
    {
        const Clip* next = demo_walking_ ? c->set->find("Walk_Loop") : c->set->find("Idle_Loop");
        if (next == nullptr) next = c->default_clip();
        if (next != nullptr) c->fade.play(next, fade_seconds);
    }
}

void anim_demo::tick(float dt, std::span<glm::mat4> palette_slot)
{
    const float fade_seconds = std::max(0.0f, cv_anim_blend().get());
    const std::string request = cv_anim_clip().get();
    if (request != active_clip_)
    {
        active_clip_ = request;
        if (!request.empty()) apply_clip_request(request, fade_seconds);
    }
    if (request.empty() && cv_anim_demo().get() != 0) advance_auto_demo(dt, fade_seconds);

    const std::span<const glm::mat4> ibm = geo_->skin_inverse_bind();
    const std::span<const uint32_t> remap = geo_->skin_joint_remap();
    for (const std::unique_ptr<Character>& c : characters_)
    {
        c->fade.advance(dt, cv_anim_rate().get());
        c->player.set_layers(c->fade.layers());
        c->player.sample();
        ::string::anim::build_palette(
            c->player.model_space(),
            remap.subspan(c->meta.remap_offset, c->meta.joint_count),
            ibm.subspan(c->meta.ibm_offset, c->meta.joint_count),
            palette_slot.subspan(c->meta.palette_offset, c->meta.joint_count));
    }
}

}  // namespace sandbox
