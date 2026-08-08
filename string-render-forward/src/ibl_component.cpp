#include <string/render/geometry/ibl_component.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include <string/gpu/command_recorder.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/pipeline_builder.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program_registry.hpp>
#include <string/core/logger.hpp>
#include <string/vulkan/vulkan_utils.hpp>

#include <glm/gtc/constants.hpp>

namespace string::render
{
using namespace string;

ibl_component::ibl_component(engine_context& ctx)
: device_(ctx.device)
, allocator_(ctx.allocator)
, descriptor_set_(ctx.descriptor_table.get_set())
{
    // The five compute pipelines, one per ibl.slang entry point (hot-reload registry). No images, no
    // samplers, no per-mip views and no bindless slots are created here: the cubemaps, the DFG LUT
    // and the SH buffer are graph resources, and every slot the pushes carry is resolved from the
    // pass's own declaration while it records.
    VkDescriptorSetLayout layout = ctx.descriptor_table.get_layout();
    const auto make_ibl_entry = [&](const char* entry) {
        return ctx.shader_registry.create(
            ctx.resources_path / "shaders" / "ibl.slang",
            [layout, entry](::string::gpu::device& dev, const ::string::gpu::compiled_program& compiled) {
                ::string::gpu::pipeline p{};
                p.push_constants = compiled.layout.push_constant;
                p.pipeline_layout = ::string::gpu::pipeline_layout_builder()
                    .set_descriptor_set_layout({ layout })
                    .set_push_constant_ranges({ compiled.layout.push_constant })
                    .build(dev);
                ::string::gpu::pipeline_builder builder(dev, ::string::gpu::pipeline_type::COMPUTE);
                for (const auto& stage : compiled.stages)
                    if (stage.stage == VK_SHADER_STAGE_COMPUTE_BIT && stage.entry_point == entry)
                        builder.add_compute_shader_spirv(stage.spirv, stage.entry_point);
                p.pipeline = builder.build_compute_pipeline(p.pipeline_layout);
                p.pipeline_type = ::string::gpu::pipeline_type::COMPUTE;
                return p;
            });
    };
    env_capture_program_ = make_ibl_entry("capture_main");
    env_mip_program_ = make_ibl_entry("mip_main");
    env_prefilter_program_ = make_ibl_entry("prefilter_main");
    sh_project_program_ = make_ibl_entry("sh_project_main");
    dfg_program_ = make_ibl_entry("dfg_main");

    STRING_LOG_INFO("[ibl] env {}px cube x{} mips (capture) / x{} mips (prefiltered ladder), "
                    "DFG {}px, L2 SH", kEnvSize, kEnvCaptureMips, kEnvPrefilterMips, kDfgSize);
}

ibl_component::~ibl_component()
{
    const auto destroy_program = [this](::string::gpu::shader_program* prog) {
        if (!prog) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_.get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_.get_device(), p.pipeline_layout, nullptr);
    };
    destroy_program(env_capture_program_);
    destroy_program(env_mip_program_);
    destroy_program(env_prefilter_program_);
    destroy_program(sh_project_program_);
    destroy_program(dfg_program_);
}

void ibl_component::tick(const IblLighting& light, bool force_every_frame)
{
    light_ = light;
    constexpr float kSunDeltaCos = 0.999998477f;   // cos(0.1 deg)
    const float align = glm::dot(glm::normalize(light.sun_dir), ibl_captured_sun_dir_);
    if (!ibl_primed_ || force_every_frame || light.furnace != ibl_captured_furnace_
        || align < kSunDeltaCos)
        ibl_update_pending_ = true;

    // One decision per frame, read by every pass's conditional. The bookkeeping that used to sit at
    // the end of record_update() belongs here with it: the chain is a CPU decision about whether to
    // record, and a pass body that mutated the decision would gate the rest of itself off.
    chain_this_frame_ = (ibl_update_pending_ || !dfg_baked_)
                     && env_capture_program_ != nullptr && env_capture_.valid();
    if (chain_this_frame_)
    {
        ibl_captured_sun_dir_ = glm::normalize(light.sun_dir);
        ibl_captured_furnace_ = light.furnace;
        ibl_primed_ = true;
        ibl_update_pending_ = false;
        ++ibl_update_count_;
    }
}

// The capture -> mip chain -> SH + prefilter ladder, as declared passes. Every ordering fact the nine
// hand-rolled barriers used to assert is a declaration here:
//   * capture mip m reads mip m-1 and writes mip m, so the chain derives from the slices themselves
//   * the SH projection names the ONE mip it reads (kShSourceMip), not the whole cube
//   * the prefilter mips write disjoint slices of the ladder and read the capture cube whole, so they
//     stay independent of each other — which is what the "mips independent" comment meant
//   * first use discards (no UNDEFINED->GENERAL prologue), the cross-frame WAR against last frame's
//     shading reads derives from tracked state, and the read edges into the lit fragments derive from
//     the consumers' own declarations
void ibl_component::declare(::string::frame_graph& fg,
                            ::string::gpu::image env_capture, ::string::gpu::image env_prefiltered,
                            ::string::gpu::image dfg_lut, ::string::gpu::buffer sh)
{
    env_capture_ = env_capture;
    env_prefiltered_ = env_prefiltered;
    dfg_lut_ = dfg_lut;
    sh_buffer_ = sh;

    const auto chain = [this] { return chain_this_frame_; };

    // Split-sum BRDF LUT: one-time, independent of the sky.
    fg.pass("ibl.dfg")
      .writes(dfg_lut_)
      .toggle([this] { return !dfg_baked_; })
      .compute([this](::string::pass_context& ctx) { record_dfg(ctx); });

    // 1) Sky -> capture mip 0 (all six faces).
    fg.pass("ibl.capture")
      .writes(env_capture_.mip(0))
      .toggle(chain)
      .compute([this](::string::pass_context& ctx) { record_capture(ctx); });

    // 2) Capture average chain (PDF-mip source + SH source), one pass per mip.
    for (uint32_t m = 1; m < kEnvCaptureMips; ++m)
    {
        fg.pass("ibl.capture.mip" + std::to_string(m))
          .reads(env_capture_.mip(m - 1), ::string::access::storage_image_read)
          .writes(env_capture_.mip(m))
          .toggle(chain)
          .compute([this, m](::string::pass_context& ctx) { record_capture_mip(ctx, m); });
    }

    // 3) L2 SH projection from exactly one capture mip.
    fg.pass("ibl.sh_project")
      .reads(env_capture_.mip(kShSourceMip), ::string::access::storage_image_read)
      .writes(sh_buffer_)
      .toggle(chain)
      .compute([this](::string::pass_context& ctx) { record_sh_project(ctx); });

    // 4) GGX prefilter ladder: each mip samples the whole capture cube (computed LOD) and writes its
    //    own slice of the ladder — six disjoint writes, no derived serialisation between them.
    for (uint32_t m = 0; m < kEnvPrefilterMips; ++m)
    {
        fg.pass("ibl.prefilter." + std::to_string(m))
          .reads(env_capture_)
          .writes(env_prefiltered_.mip(m))
          .toggle(chain)
          .compute([this, m](::string::pass_context& ctx) { record_prefilter(ctx, m); });
    }
}

IblPush ibl_component::base_push() const
{
    IblPush push{};
    push.sun_dir = glm::vec4(glm::normalize(light_.sun_dir), light_.furnace ? 1.0f : 0.0f);
    push.sky_zenith = glm::vec4(light_.sky_zenith, 0.0f);
    push.sky_ground = glm::vec4(light_.sky_ground, 0.0f);
    push.sun_color = glm::vec4(light_.sun_color, light_.sun_intensity);   // w: klx (ground-band lighting)
    return push;
}

void ibl_component::dispatch(::string::pass_context& ctx, ::string::gpu::shader_program* prog,
                             const IblPush& push, uint32_t gx, uint32_t gy, uint32_t gz) const
{
    const ::string::gpu::pipeline& p = prog->current();
    ctx.rec.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    ctx.rec.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                 0, 1, &descriptor_set_, 0, nullptr);
    ctx.rec.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(IblPush), &push);
    ctx.rec.dispatch(gx, gy, gz);
}

void ibl_component::record_dfg(::string::pass_context& ctx)
{
    IblPush push = base_push();
    push.dst_slot = ctx.slot(dfg_lut_);
    push.dst_size = kDfgSize;
    push.sample_count = kDfgSamples;
    dispatch(ctx, dfg_program_, push, (kDfgSize + 7) / 8, (kDfgSize + 7) / 8, 1);
    dfg_baked_ = true;
}

void ibl_component::record_capture(::string::pass_context& ctx)
{
    IblPush push = base_push();
    push.dst_slot = ctx.slot(env_capture_.mip(0));
    push.dst_size = kEnvSize;
    dispatch(ctx, env_capture_program_, push, kEnvSize / 8, kEnvSize / 8, 6);
}

void ibl_component::record_capture_mip(::string::pass_context& ctx, uint32_t mip)
{
    IblPush push = base_push();
    push.src_slot = ctx.slot(env_capture_.mip(mip - 1));
    push.dst_slot = ctx.slot(env_capture_.mip(mip));
    push.src_size = kEnvSize >> (mip - 1);
    push.dst_size = kEnvSize >> mip;
    dispatch(ctx, env_mip_program_, push, (push.dst_size + 7) / 8, (push.dst_size + 7) / 8, 6);
}

void ibl_component::record_sh_project(::string::pass_context& ctx)
{
    IblPush push = base_push();
    push.src_slot = ctx.slot(env_capture_.mip(kShSourceMip));
    push.src_size = kEnvSize >> kShSourceMip;
    push.sh = ctx.address(sh_buffer_);
    dispatch(ctx, sh_project_program_, push, 1, 1, 1);
}

void ibl_component::record_prefilter(::string::pass_context& ctx, uint32_t mip)
{
    IblPush push = base_push();
    push.src_slot = ctx.slot(env_capture_);            // SamplerCube: the whole capture chain
    push.src_size = kEnvSize;
    push.dst_slot = ctx.slot(env_prefiltered_.mip(mip));
    push.dst_size = kEnvSize >> mip;
    push.roughness = float(mip) / float(kEnvPrefilterMips - 1);
    push.sample_count = kPrefilterSamples;
    push.mip_count = kEnvCaptureMips;
    dispatch(ctx, env_prefilter_program_, push, (push.dst_size + 7) / 8, (push.dst_size + 7) / 8, 6);
}

namespace
{

float half_to_float(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? std::numeric_limits<float>::quiet_NaN()
                               : std::numeric_limits<float>::infinity();
    else v = std::ldexp(static_cast<float>(man + 1024), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// CPU reference for the split-sum DFG integral — the same estimator (Hammersley + GGX importance
// sampling + height-correlated Smith visibility) as dfg_main in ibl.slang, in double precision.
glm::dvec2 dfg_reference(double NdotV, double perceptual, uint32_t samples)
{
    const double alpha = perceptual * perceptual;
    const glm::dvec3 V(std::sqrt(std::max(1.0 - NdotV * NdotV, 0.0)), 0.0, NdotV);
    double a = 0.0, b = 0.0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        uint32_t bits = i;
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
        bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
        bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
        bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
        const glm::dvec2 xi(double(i) / samples, double(bits) * 2.3283064365386963e-10);
        const double phi = 2.0 * glm::pi<double>() * xi.x;
        const double ct = std::sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
        const double st = std::sqrt(std::max(1.0 - ct * ct, 0.0));
        const glm::dvec3 H(st * std::cos(phi), st * std::sin(phi), ct);
        const glm::dvec3 L = 2.0 * glm::dot(V, H) * H - V;
        if (L.z <= 0.0) continue;
        const double NdotL = L.z;
        const double NdotH = std::max(H.z, 0.0);
        const double VdotH = std::max(glm::dot(V, H), 1e-4);
        const double a2 = alpha * alpha;
        const double gv = NdotL * std::sqrt(NdotV * NdotV * (1.0 - a2) + a2);
        const double gl = NdotV * std::sqrt(NdotL * NdotL * (1.0 - a2) + a2);
        const double vis = 0.5 / std::max(gv + gl, 1e-7);
        const double g_vis = 4.0 * vis * VdotH * NdotL / std::max(NdotH, 1e-4);
        const double fc = std::pow(1.0 - VdotH, 5.0);
        a += (1.0 - fc) * g_vis;
        b += fc * g_vis;
    }
    return { a / samples, b / samples };
}

}  // namespace

// Debug only, and deliberately UNCHANGED: it drains the device and runs on its own immediate submit,
// so it is outside the frame graph rather than an exception to it. (The sh_buffer copy below still
// has no barrier of its own and relies entirely on that device idle — as before.)
void ibl_component::run_verification(bool furnace, ::string::gpu::resource_id sh_buffer,
                                     ::string::gpu::resource_id dfg_lut)
{
    if (sh_buffer == 0 || dfg_lut == 0) return;
    vkDeviceWaitIdle(device_.get_device());

    const VkDeviceSize sh_bytes = sizeof(float) * 4 * kShCoefficients;
    const VkDeviceSize dfg_bytes = VkDeviceSize(kDfgSize) * kDfgSize * 8;   // RGBA16F
    const ::string::gpu::resource_id staging = allocator_.create_resource(::string::gpu::buffer_info{
        .size = sh_bytes + dfg_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    });
    {
        ::string::gpu::command_recorder rec;
        rec.init(device_.get_device(), device_.get_queue(::string::gpu::queue_type::GRAPHICS));
        VkCommandBuffer cb = rec.begin();
        const VkBufferCopy sh_region = { 0, 0, sh_bytes };
        vkCmdCopyBuffer(cb, allocator_.get_buffer(sh_buffer).buffer,
                        allocator_.get_buffer(staging).buffer, 1, &sh_region);
        const ::string::gpu::allocated_image& dfg = allocator_.get_image(dfg_lut);
        string::vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .src_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        const VkBufferImageCopy dfg_region = {
            .bufferOffset = sh_bytes,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { kDfgSize, kDfgSize, 1 },
        };
        vkCmdCopyImageToBuffer(cb, dfg.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               allocator_.get_buffer(staging).buffer, 1, &dfg_region);
        string::vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .src_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        rec.end().immediate_submit();
        rec.destroy();
    }

    const uint8_t* mapped = static_cast<const uint8_t*>(
        allocator_.get_buffer(staging).allocation_info.pMappedData);
    const float* sh = reinterpret_cast<const float*>(mapped);
    const uint16_t* dfg = reinterpret_cast<const uint16_t*>(mapped + sh_bytes);

    bool pass = true;

    // --- SH checks --------------------------------------------------------------------------
    const auto sh_c = [&](int i) { return glm::vec3(sh[i * 4 + 0], sh[i * 4 + 1], sh[i * 4 + 2]); };
    const auto e_over_pi = [&](const glm::vec3& n) {
        return sh_c(0) * 0.282095f
             + sh_c(1) * (0.488603f * n.y) + sh_c(2) * (0.488603f * n.z) + sh_c(3) * (0.488603f * n.x)
             + sh_c(4) * (1.092548f * n.x * n.y) + sh_c(5) * (1.092548f * n.y * n.z)
             + sh_c(6) * (0.315392f * (3.0f * n.z * n.z - 1.0f))
             + sh_c(7) * (1.092548f * n.x * n.z)
             + sh_c(8) * (0.546274f * (n.x * n.x - n.y * n.y));
    };
    if (furnace)
    {
        const float dc = sh_c(0).r * 0.282095f;
        float residual = 0.0f;
        for (int i = 1; i < 9; ++i)
            residual = std::max(residual, std::max(std::abs(sh_c(i).r),
                        std::max(std::abs(sh_c(i).g), std::abs(sh_c(i).b))));
        const bool ok = std::abs(dc - 1.0f) < 0.02f && residual < 0.02f;
        pass = pass && ok;
        STRING_LOG_INFO("[ibl-verify] furnace SH: DC E/pi = {:.5f} (expect 1.0), max |l>0| = {:.5f} -> {}",
                        dc, residual, ok ? "PASS" : "FAIL");
    }
    else
    {
        const glm::vec3 up = e_over_pi(glm::vec3(0, 1, 0));
        const glm::vec3 down = e_over_pi(glm::vec3(0, -1, 0));
        const bool ok = std::isfinite(up.r + up.g + up.b) && up.g > down.g && down.g >= -0.05f;
        pass = pass && ok;
        STRING_LOG_INFO("[ibl-verify] sky SH: E/pi(+Y) = ({:.3f},{:.3f},{:.3f}), E/pi(-Y) = "
                        "({:.3f},{:.3f},{:.3f}) -> {}",
                        up.r, up.g, up.b, down.r, down.g, down.b, ok ? "PASS" : "FAIL");
    }

    // --- DFG checks -------------------------------------------------------------------------
    const auto dfg_at = [&](uint32_t x, uint32_t y) {
        const uint16_t* t = dfg + (VkDeviceSize(y) * kDfgSize + x) * 4;
        return glm::vec2(half_to_float(t[0]), half_to_float(t[1]));
    };
    float sum_max = 0.0f, sum_min = 10.0f;
    for (uint32_t y = 0; y < kDfgSize; ++y)
        for (uint32_t x = 0; x < kDfgSize; ++x)
        {
            const glm::vec2 v = dfg_at(x, y);
            sum_max = std::max(sum_max, v.x + v.y);
            sum_min = std::min(sum_min, v.x + v.y);
        }
    const bool bounded = sum_max <= 1.01f && sum_min > 0.0f;
    pass = pass && bounded;
    STRING_LOG_INFO("[ibl-verify] DFG A+B range [{:.4f}, {:.4f}] (expect (0, 1.01]) -> {}",
                    sum_min, sum_max, bounded ? "PASS" : "FAIL");
    const glm::vec2 probes[5] = { { 0.5f, 0.5f }, { 0.9f, 0.1f }, { 0.2f, 0.8f },
                                  { 0.7f, 0.3f }, { 0.95f, 0.95f } };
    for (const glm::vec2& p : probes)
    {
        const uint32_t x = std::min(kDfgSize - 1, uint32_t(p.x * kDfgSize));
        const uint32_t y = std::min(kDfgSize - 1, uint32_t(p.y * kDfgSize));
        const double nv = (x + 0.5) / kDfgSize;
        const double r = (y + 0.5) / kDfgSize;
        const glm::vec2 gpu = dfg_at(x, y);
        const glm::dvec2 ref = dfg_reference(nv, r, kDfgSamples);
        const bool ok = std::abs(gpu.x - ref.x) < 0.02 && std::abs(gpu.y - ref.y) < 0.02;
        pass = pass && ok;
        STRING_LOG_INFO("[ibl-verify] DFG({:.2f},{:.2f}): gpu ({:.4f},{:.4f}) ref ({:.4f},{:.4f}) -> {}",
                        nv, r, gpu.x, gpu.y, ref.x, ref.y, ok ? "PASS" : "FAIL");
    }

    STRING_LOG_INFO("[ibl-verify] overall: {}", pass ? "PASS" : "FAIL");
    allocator_.destroy_resource(staging);
}

}  // namespace string::render
