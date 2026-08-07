#include <string/vulkan/passes/composite_pass.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <string/core/cache_dir.hpp>
#include <string/core/cvar.hpp>
#include <string/core/logger.hpp>

namespace String
{

namespace
{

// Brief 07: manual EV100 exposure. Brief 09 layers histogram auto-exposure on top: the post
// chain's metering publishes a smoothed EV100 (set_auto_ev100) that takes over while
// r.exposure.auto is on; this CVar remains the manual override / pre-warmup value.
// Scene radiometric units are KILO-nits-scale (1 unit = 1000 cd/m^2 — the brief-07 "physical-ish"
// unit convention), so the standard saturation-based exposure H = 1/(1.2 * 2^EV100) picks up a
// x1000 unit factor here. That factor IS the pre-exposure that keeps midday-sun values well
// inside HDR16F range.
string::core::CVar<float>& ev100_cvar()
{
    // Default 14.6 = sunny-16, exact for the brief-07 unit convention: noon sun 100 klx -> an
    // 18% grey card in full sun lands on middle grey after the ACES curve (verified against the
    // lookdev captures). Interiors/dusk are correspondingly darker under manual exposure.
    static string::core::CVar<float> v{"r.exposure.ev100", 14.6f,
        "manual exposure value (EV100); higher = darker. 14.6 = sunny-16 (noon calibration)"};
    static const bool aliased = [] { v.add_alias("ev100"); return true; }();
    (void)aliased;
    return v;
}

string::core::CVar<bool>& exposure_auto_cvar()
{
    static string::core::CVar<bool> v{"r.exposure.auto", false,
        "histogram auto-exposure drives EV100 (default OFF; manual r.exposure.ev100 is the "
        "default path — enable to opt in to metering)"};
    static const bool aliased = [] { v.add_alias("exposure_auto"); return true; }();
    (void)aliased;
    return v;
}

// Brief 09 output transform + grading CVars (baked into the LUT; re-bake on change).
string::core::CVar<std::string>& tonemap_cvar()
{
    static string::core::CVar<std::string> v{"r.tonemap", "aces2",
        "output transform curve: aces2 (CAM DRT LUT) or aces1 (v1 fitted curve, A/B)"};
    static const bool aliased = [] { v.add_alias("tonemap"); return true; }();
    (void)aliased;
    return v;
}
string::core::CVar<int32_t>& lut_size_cvar()
{
    // 96 (raised from 48 via 64, 2026-07-24): the trilinear LUT approximates the transform
    // piecewise-linearly, and in DARK gradient regions (sub-horizon sky) the segment kinks at
    // 48^3 reached a couple of encoded LSBs — visible banding the half-LSB output dither cannot
    // cover. User-verified sub-perceptual at 96 (the clamp ceiling; a requested "128" clamps to
    // 96). The ~3 s bake this costs is paid once per parameter set — baked LUTs are cached on
    // disk (user_cache_dir("tonemap")), so subsequent launches load in milliseconds.
    static string::core::CVar<int32_t> v{"r.tonemap.lut_size", 96,
        "output-transform 3D LUT edge size (baked as a 2D strip; clamped to 16..96)"};
    static const bool aliased = [] { v.add_alias("lut_size"); return true; }();
    (void)aliased;
    return v;
}
string::core::CVar<float>& grade_exposure_cvar()
{
    static string::core::CVar<float> v{"r.grade.exposure", 0.0f, "grading exposure trim (stops)"};
    static const bool a = [] { v.add_alias("grade_exposure"); return true; }(); (void)a;
    return v;
}
string::core::CVar<float>& grade_contrast_cvar()
{
    static string::core::CVar<float> v{"r.grade.contrast", 1.0f, "grading log contrast (pivot 0.18)"};
    static const bool a = [] { v.add_alias("grade_contrast"); return true; }(); (void)a;
    return v;
}
string::core::CVar<float>& grade_saturation_cvar()
{
    static string::core::CVar<float> v{"r.grade.saturation", 1.0f, "grading saturation"};
    static const bool a = [] { v.add_alias("grade_saturation"); return true; }(); (void)a;
    return v;
}
string::core::CVar<float>& grade_temperature_cvar()
{
    static string::core::CVar<float> v{"r.grade.temperature", 0.0f, "white balance temperature [-1,1]"};
    static const bool a = [] { v.add_alias("grade_temperature"); return true; }(); (void)a;
    return v;
}
string::core::CVar<float>& grade_tint_cvar()
{
    static string::core::CVar<float> v{"r.grade.tint", 0.0f, "white balance tint [-1,1]"};
    static const bool a = [] { v.add_alias("grade_tint"); return true; }(); (void)a;
    return v;
}
string::core::CVar<float>& grade_lift_cvar()
{
    static string::core::CVar<float> v{"r.grade.lift", 0.0f, "grading lift (shaper space)"};
    static const bool a = [] { v.add_alias("grade_lift"); return true; }(); (void)a;
    return v;
}
string::core::CVar<float>& grade_gamma_cvar()
{
    static string::core::CVar<float> v{"r.grade.gamma", 1.0f, "grading gamma (shaper space)"};
    static const bool a = [] { v.add_alias("grade_gamma"); return true; }(); (void)a;
    return v;
}
string::core::CVar<float>& grade_gain_cvar()
{
    static string::core::CVar<float> v{"r.grade.gain", 1.0f, "grading gain (shaper space)"};
    static const bool a = [] { v.add_alias("grade_gain"); return true; }(); (void)a;
    return v;
}

string::core::tonemap::Grading grading_from_cvars()
{
    string::core::tonemap::Grading g;
    g.exposure_stops = grade_exposure_cvar().get();
    g.contrast = grade_contrast_cvar().get();
    g.saturation = grade_saturation_cvar().get();
    g.temperature = grade_temperature_cvar().get();
    g.tint = grade_tint_cvar().get();
    g.lift = grade_lift_cvar().get();
    g.gamma = grade_gamma_cvar().get();
    g.gain = grade_gain_cvar().get();
    return g;
}

string::core::tonemap::Curve curve_from_cvar()
{
    return tonemap_cvar().get() == "aces1" ? string::core::tonemap::Curve::Aces1
                                           : string::core::tonemap::Curve::Aces2;
}

uint16_t float_to_half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) return uint16_t(sign);                       // flush denormals/underflow to 0
    if (exp >= 31) return uint16_t(sign | 0x7BFFu);            // clamp to max half
    return uint16_t(sign | (uint32_t(exp) << 10) | (man >> 13));
}

// The active pass instance (there is exactly one, renderer-owned) + the published auto EV, for
// the static exposure_scale()/encode_display() the capture writer shares.
composite_pass* s_active = nullptr;
const std::vector<float>* s_lut = nullptr;
uint32_t s_lut_size = 0;
string::core::tonemap::Curve s_curve{};
string::core::tonemap::Grading s_grading{};
float s_auto_ev100 = 0.0f;
bool s_auto_valid = false;
float s_override_ev100 = 0.0f;
bool s_override_active = false;

}  // namespace

composite_pass::composite_pass(String::engine_context& ctx, VkFormat color_format)
: device_(ctx.device)
, allocator_(ctx.allocator)
, descriptor_table_(ctx.descriptor_table)
{
    const std::filesystem::path resources_path = ctx.resources_path;
    string::gpu::shader_program_registry& registry = ctx.shader_registry;
    VkDescriptorSetLayout global_layout = descriptor_table_.get_layout();
    // The pipeline is built from compiled Slang + reflection and owned by the shader_program, so a
    // save recompiles + swaps it. Set 0 stays the real bindless table layout (reflection can't
    // reproduce its update-after-bind/variable-count flags); reflection drives the push-constant
    // range (offset/size/stages) — the layout plumbing that actually varies per pass.
    program_ = registry.create(
        resources_path / "shaders" / "composite.slang",
        [global_layout, color_format](string::gpu::device& dev,
                                      const string::gpu::compiled_program& compiled) {
            string::gpu::pipeline p{};
            const VkPushConstantRange range = compiled.layout.has_push_constant
                ? compiled.layout.push_constant
                : VkPushConstantRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(uint32_t) };

            p.push_constants = range;
            p.pipeline_layout = string::gpu::pipeline_layout_builder()
                .set_descriptor_set_layout({ global_layout })
                .set_push_constant_ranges({ range })
                .build(dev);

            string::gpu::pipeline_builder builder(dev);
            for (const auto& stage : compiled.stages)
            {
                if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT)
                    builder.add_vertex_shader_spirv(stage.spirv, stage.entry_point);
                else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    builder.add_fragment_shader_spirv(stage.spirv, stage.entry_point);
            }
            // Fullscreen triangle sampling the bindless HDR target into the swapchain: no depth,
            // no blending, target format is the swapchain's.
            p.pipeline = builder
                .set_input_assembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                .set_tessellation()
                .set_rasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .set_multisampling()
                .disable_color_blending()
                .set_color_format(color_format)
                .build_graphics_pipeline(p.pipeline_layout);
            p.pipeline_type = string::gpu::pipeline_type::GRAPHICS;
            return p;
        });

    // Touch the exposure/tonemap/grading CVars so they are registered before the renderer's
    // apply_env() (the pass is a renderer member, constructed first) — the STRING_* env levers
    // must work headlessly. The LUT itself bakes AFTER apply_env, on the first update().
    ev100_cvar();
    exposure_auto_cvar();
    tonemap_cvar();
    lut_size_cvar();
    grading_from_cvars();

    // LUT sampler: linear, clamp (the strip must not wrap at slice edges beyond the manual
    // within-slice coordinate math in composite.slang).
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(device_.get_device(), &sampler_info, nullptr, &lut_sampler_) != VK_SUCCESS)
        throw std::runtime_error("composite_pass: failed to create LUT sampler");

    s_active = this;
}

composite_pass::~composite_pass()
{
    s_active = nullptr;
    s_lut = nullptr;
    if (lut_image_ != 0)
    {
        descriptor_table_.unbind(lut_image_, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(lut_image_);
    }
    vkDestroySampler(device_.get_device(), lut_sampler_, nullptr);
    const string::gpu::pipeline& p = program_->current();
    const VkDescriptorSet set = descriptor_table_.get_set();
    vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
    vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
}

void composite_pass::bake_and_upload_lut(bool first)
{
    using namespace string::core::tonemap;
    const auto t0 = std::chrono::steady_clock::now();
    baked_curve_ = curve_from_cvar();
    baked_grading_ = grading_from_cvars();
    const uint32_t size = uint32_t(std::clamp(lut_size_cvar().get(), 16, 96));

    // Disk cache: at the 96^3 default the bake costs seconds; the result is a pure function of
    // (curve, grading, size), so cache it under user_cache_dir keyed by exactly those inputs
    // (+ a format version). A cache hit turns the init bake into a millisecond file read.
    std::string cache_key;
    {
        char buf[256];
        const auto& g = baked_grading_;
        std::snprintf(buf, sizeof buf, "v1_%d_%u_%.6g_%.6g_%.6g_%.6g_%.6g_%.6g_%.6g_%.6g",
                      int(baked_curve_), size, double(g.exposure_stops), double(g.contrast),
                      double(g.saturation), double(g.temperature), double(g.tint),
                      double(g.lift), double(g.gamma), double(g.gain));
        cache_key = buf;
    }
    const std::filesystem::path cache_file =
        string::core::user_cache_dir("tonemap") / (cache_key + ".lut");
    const size_t expect_floats = size_t(size) * size * size * 3;
    bool cache_hit = false;
    if (std::ifstream in{cache_file, std::ios::binary}; in)
    {
        lut_cpu_.resize(expect_floats);
        in.read(reinterpret_cast<char*>(lut_cpu_.data()),
                std::streamsize(expect_floats * sizeof(float)));
        cache_hit = in.good() && size_t(in.gcount()) == expect_floats * sizeof(float);
    }
    if (!cache_hit)
    {
        lut_cpu_ = bake_lut(baked_curve_, baked_grading_, size);
        std::error_code ec;
        std::filesystem::create_directories(cache_file.parent_path(), ec);
        if (!ec)
        {
            std::ofstream out{cache_file, std::ios::binary | std::ios::trunc};
            out.write(reinterpret_cast<const char*>(lut_cpu_.data()),
                      std::streamsize(expect_floats * sizeof(float)));
        }
    }

    const uint32_t width = size * size, height = size;
    if (!first && size != lut_size_ && lut_image_ != 0)
    {
        // Size change: retire the old image (callers already drained the device).
        descriptor_table_.unbind(lut_image_, string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(lut_image_);
        lut_image_ = 0;
    }
    lut_size_ = size;
    if (lut_image_ == 0)
    {
        lut_image_ = allocator_.create_resource(string::gpu::image_info{
            .extent = { width, height, 1 },
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
        });
        descriptor_table_.bind(lut_image_, string::gpu::descriptor_type::TEXTURE);
        lut_slot_ = descriptor_table_.get_binding_slot(lut_image_, string::gpu::descriptor_type::TEXTURE);
        }

    // Upload: RGBA16F staging (alpha = 1), one copy, park in SHADER_READ_ONLY. Init-time or
    // behind a device drain (rebake), so an immediate submit is fine.
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 8;
    const string::gpu::resource_id staging = allocator_.create_resource(string::gpu::buffer_info{
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    });
    uint16_t* dst = static_cast<uint16_t*>(allocator_.get_buffer(staging).allocation_info.pMappedData);
    const uint16_t one = float_to_half(1.0f);
    for (size_t i = 0; i < size_t(width) * height; ++i)
    {
        dst[i * 4 + 0] = float_to_half(lut_cpu_[i * 3 + 0]);
        dst[i * 4 + 1] = float_to_half(lut_cpu_[i * 3 + 1]);
        dst[i * 4 + 2] = float_to_half(lut_cpu_[i * 3 + 2]);
        dst[i * 4 + 3] = one;
    }

    string::gpu::command_recorder recorder;
    recorder.init(device_.get_device(), device_.get_queue(string::gpu::queue_type::GRAPHICS));
    VkCommandBuffer cb = recorder.begin();
    const string::gpu::allocated_image& img = allocator_.get_image(lut_image_);
    vku::transition_image(cb, {
        .image = img.image,
        .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .new_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
        .dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });
    const VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyBufferToImage(cb, allocator_.get_buffer(staging).buffer, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vku::transition_image(cb, {
        .image = img.image,
        .old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    });
    recorder.end().immediate_submit();
    recorder.destroy();
    allocator_.destroy_resource(staging);

    s_lut = &lut_cpu_;
    s_lut_size = lut_size_;
    s_curve = baked_curve_;
    s_grading = baked_grading_;

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    STRING_LOG_INFO("[tonemap] {} {} LUT {}^3 ({}x{} strip) in {} ms{}",
                    cache_hit ? "cache-loaded" : "baked",
                    baked_curve_ == string::core::tonemap::Curve::Aces2 ? "aces2" : "aces1",
                    size, width, height, ms,
                    baked_grading_.neutral() ? "" : " (graded)");
}

float composite_pass::exposure_scale()
{
    // Calibration override (white furnace) beats both auto and manual — the gate needs a pinned,
    // scene-independent exposure.
    const float ev100 = s_override_active
        ? s_override_ev100
        : (exposure_auto_cvar().get() && s_auto_valid) ? s_auto_ev100 : ev100_cvar().get();
    return 1000.0f / (1.2f * std::exp2(ev100));
}

void composite_pass::set_exposure_override(float ev100, bool active)
{
    s_override_ev100 = ev100;
    s_override_active = active;
}

void composite_pass::set_auto_ev100(float ev100)
{
    s_auto_ev100 = ev100;
    s_auto_valid = true;
}

bool composite_pass::auto_exposure_enabled()
{
    return exposure_auto_cvar().get();
}

glm::vec3 composite_pass::encode_display(glm::vec3 hdr)
{
    const glm::vec3 exposed = glm::max(hdr, glm::vec3(0.0f)) * exposure_scale();
    if (s_lut && !s_lut->empty())
        return string::core::tonemap::sample_lut(*s_lut, s_lut_size, exposed);
    // Pre-first-bake fallback (never hit in a normal frame loop): the exact transform.
    return string::core::tonemap::transform(s_curve, s_grading, exposed);
}

void composite_pass::tick()
{
    // First bake happens here (after apply_env: STRING_TONEMAP / STRING_GRADE_* honoured);
    // parameter changes re-bake behind a device drain — amortized like the sky IBL: nothing on
    // the steady-state frame, a logged hitch when a grading/tonemap CVar changes.
    const bool first = lut_image_ == 0;
    if (!first
        && curve_from_cvar() == baked_curve_
        && grading_from_cvars() == baked_grading_
        && uint32_t(std::clamp(lut_size_cvar().get(), 16, 96)) == lut_size_)
        return;
    if (!first) vkDeviceWaitIdle(device_.get_device());
    bake_and_upload_lut(first);
}

// Author onto the graph. The declaration says only WHAT is touched: a sampled read of the scene
// colour and a colour write to the target. Load/store, the barrier that makes the read safe, and the
// layout both images must be in are all derived from that plus where this pass lands in the order.
void composite_pass::declare(string::frame_graph& fg, string::gpu::image hdr, string::gpu::image target)
{
    fg.pass("composite")
      .reads(hdr)
      .color(target)
      .raster([this, hdr](string::pass_context& ctx) { record(ctx, hdr); });
}

void composite_pass::record(string::pass_context& ctx, string::gpu::image hdr)
{
    string::gpu::command_recorder& recorder = ctx.rec;
    const string::gpu::pipeline& p = program_->current();
    const VkDescriptorSet set = descriptor_table_.get_set();

    recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_GRAPHICS,
        p.pipeline_layout, 0, 1, &set, 0, nullptr);
    // { source_slot, exposure, lut_slot, lut_size } — exposure scales the HDR into the LUT's
    // shaper domain; the LUT applies grading + the output transform (see composite.slang).
    //
    // Brief 14 M5: the lens array rides in the SAME push block, packed to 16 bytes per lens. Four
    // lenses at one float4 each is 64 bytes on top of the 24-byte header — comfortably inside the
    // 128-byte guaranteed minimum, where an unpacked layout (rect + magnification + slot + flags as
    // separate floats) would have landed exactly ON it with no headroom.
    struct Push
    {
        uint32_t source_slot;
        float exposure;
        uint32_t lut_slot;
        uint32_t lut_size;
        uint32_t lens_count;
        uint32_t _pad[3];
        uint32_t lenses[LensState::kMaxLenses][4];
    } push{};
    push.source_slot = ctx.slot(hdr);
    push.exposure = exposure_scale();
    push.lut_slot = lut_slot_;
    push.lut_size = lut_size_;

    const LensState& ls = LensState::instance();
    push.lens_count = std::min(ls.count, LensState::kMaxLenses);
    for (uint32_t i = 0; i < push.lens_count; ++i)
    {
        const Lens& l = ls.lenses[i];
        // Screen coords pack into 16 bits each (no display is 65k px wide); negatives are clamped
        // away because a lens is only meaningful where it overlaps the screen.
        const auto u16 = [](float v) {
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 65535.0f));
        };
        float mag = l.magnification;
        std::memcpy(&push.lenses[i][3], &mag, sizeof(float));
        push.lenses[i][0] = u16(l.x) | (u16(l.y) << 16);
        push.lenses[i][1] = u16(l.w) | (u16(l.h) << 16);
        push.lenses[i][2] = (l.source_slot & 0xFFFFu) | (l.bypass_tonemap ? 0x10000u : 0u);
    }
    recorder.push_constants(p.pipeline_layout, p.push_constants.stageFlags, 0, sizeof(push), &push);
    recorder.draw(3, 1, 0, 0);
}

LensState& LensState::instance()
{
    // Function-local static: one instance, initialised on first touch, no static-init-order
    // question between the debug UI that writes it and the pass that reads it.
    static LensState state;
    return state;
}

}  // namespace String
