#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace string::render
{
class geometry_pass;
}

namespace sandbox
{

// Brief 23 M4: the demo's animation driver — one AnimPlayer + CrossFade per skin instance the
// geometry pass loaded, palettes written into the app's ring each tick. This is app code by
// design: the engine samples and blends (string::anim); WHAT plays comes from the anim.* cvars
// or the auto idle/walk cross-fade. That is the spec's state-machine boundary — this type
// decides nothing beyond "which clip did the user ask for".
class anim_demo
{
public:
    // Loads each skin's `.anim` pack (skeleton-hash checked: a mismatch WARNs once and the
    // character stays in bind pose — the ring's identity default — never garbage).
    explicit anim_demo(const ::string::render::geometry_pass& geo);
    ~anim_demo();

    bool empty() const { return characters_.empty(); }

    // Advance clips + the fade, sample, blend, and build every character's palette window into
    // `palette_slot` — the CURRENT frame slot's mapped ring (sized palette_joints_total()).
    void tick(float dt, std::span<glm::mat4> palette_slot);

private:
    struct Character;

    void apply_clip_request(const std::string& request, float fade_seconds);
    void advance_auto_demo(float dt, float fade_seconds);

    const ::string::render::geometry_pass* geo_ = nullptr;
    std::vector<std::unique_ptr<Character>> characters_;
    std::string active_clip_;
    float demo_timer_ = 0.0f;
    bool demo_walking_ = false;
};

}  // namespace sandbox
