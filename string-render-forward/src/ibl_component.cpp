#include <string/render/geometry/ibl_component.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

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

void IblComponent::init(String::engine_context& context)
{
    device_ = &context.device;
    allocator_ = &context.allocator;
    table_ = &context.descriptor_table;

    // Linear clamp-to-edge trilinear sampler: the prefilter ladder interpolates between roughness
    // mips, and the DFG LUT must not wrap at NdotV/roughness extremes (the allocator's default
    // sampler REPEATs).
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };
    if (vkCreateSampler(device_->get_device(), &sampler_info, nullptr, &env_sampler_) != VK_SUCCESS)
        throw std::runtime_error("GeometryPass: failed to create env sampler");

    const auto make_cube = [&](uint32_t mips) {
        return allocator_->create_resource(::string::gpu::image_info{
            .extent = { kEnvSize, kEnvSize, 1 },
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
            .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
            .allocation_flags = {},
            .mip_levels = mips,
            .cube = true,
        });
    };
    env_capture_ = make_cube(kEnvCaptureMips);
    env_prefiltered_ = make_cube(kEnvPrefilterMips);

    // Sampled (SamplerCube) slots + per-mip 2D_ARRAY storage views for the compute writes.
    const auto bind_cube = [&](::string::gpu::resource_id id, uint32_t mips,
                               std::vector<VkImageView>& views, std::vector<uint32_t>& slots) {
        const ::string::gpu::allocated_image& img = allocator_->get_image(id);
        table_->bind(id, ::string::gpu::descriptor_type::TEXTURE);
        const uint32_t sample_slot =
            table_->get_binding_slot(id, ::string::gpu::descriptor_type::TEXTURE);
        table_->update_texture(sample_slot, img.view, env_sampler_);
        for (uint32_t m = 0; m < mips; ++m)
        {
            const VkImageViewCreateInfo vi = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = img.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 6 },
            };
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device_->get_device(), &vi, nullptr, &view) != VK_SUCCESS)
                throw std::runtime_error("GeometryPass: failed to create env mip view");
            views.push_back(view);
            slots.push_back(table_->bind_storage_view(view));
        }
        return sample_slot;
    };
    env_capture_sample_slot_ = bind_cube(env_capture_, kEnvCaptureMips,
                                         env_capture_mip_views_, env_capture_mip_slots_);
    env_prefiltered_slot_ = bind_cube(env_prefiltered_, kEnvPrefilterMips,
                                      env_prefiltered_mip_views_, env_prefiltered_mip_slots_);

    // DFG LUT: 2D RGBA16F (rg used), baked once; TRANSFER_SRC for the dbg.ibl_verify readback.
    dfg_lut_ = allocator_->create_resource(::string::gpu::image_info{
        .extent = { kDfgSize, kDfgSize, 1 },
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    const ::string::gpu::allocated_image& dfg = allocator_->get_image(dfg_lut_);
    table_->bind(dfg_lut_, ::string::gpu::descriptor_type::TEXTURE);
    dfg_sample_slot_ = table_->get_binding_slot(dfg_lut_, ::string::gpu::descriptor_type::TEXTURE);
    table_->update_texture(dfg_sample_slot_, dfg.view, env_sampler_);
    dfg_storage_slot_ = table_->bind_storage_view(dfg.view);

    // SH coefficients (9 x float4), written by the projection compute, read by every lit fragment.
    sh_buffer_ = allocator_->create_resource(::string::gpu::buffer_info{
        .size = sizeof(float) * 4 * 9,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
               | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });

    // The five compute pipelines, one per ibl.slang entry point (hot-reload registry).
    VkDescriptorSetLayout layout = table_->get_layout();
    const auto make_ibl_entry = [&](const char* entry) {
        return context.shader_registry.create(
            context.resources_path / "shaders" / "ibl.slang",
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

void IblComponent::begin_frame(const glm::vec3& sun_dir, bool furnace, bool force_every_frame)
{
    if (env_capture_ == 0) return;
    constexpr float kSunDeltaCos = 0.999998477f;   // cos(0.1 deg)
    const float align = glm::dot(glm::normalize(sun_dir), ibl_captured_sun_dir_);
    if (!ibl_primed_ || force_every_frame || furnace != ibl_captured_furnace_ || align < kSunDeltaCos)
        ibl_update_pending_ = true;
}

// Record the sky-IBL update chain: capture -> capture mip chain -> SH projection + GGX prefilter
// ladder. Runs only on frames where the sun moved past the trigger — the whole chain is a
// single-frame update, so the ambient is always self-consistent (no popping). The DFG LUT bake
// rides the first call. All barriers here are the documented INTRA-pass class (like the HiZ mip
// chain): everything is produced and consumed by this pass; the SH buffer's fragment-read edge is
// graph-declared (usages) and the final memory barrier makes the image writes visible to the
// fragment stage.
void IblComponent::record_update(::string::gpu::command_recorder& recorder, const IblLighting& light)
{
    VkCommandBuffer cb = recorder.vk();   // escape for the vku::transition_image calls in the mip/copy tail
    VkDescriptorSet set = table_->get_set();
    const auto dispatch = [&](::string::gpu::shader_program* prog, const IblPush& push,
                              uint32_t gx, uint32_t gy, uint32_t gz) {
        const ::string::gpu::pipeline& p = prog->current();
        recorder.bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        recorder.bind_descriptor_sets(VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout,
                                      0, 1, &set, 0, nullptr);
        recorder.push_constants(p.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(IblPush), &push);
        recorder.dispatch(gx, gy, gz);
    };
    const auto compute_barrier = [&](VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        const VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
        recorder.barrier(dep);
    };

    IblPush push{};
    push.sun_dir = glm::vec4(glm::normalize(light.sun_dir), light.furnace ? 1.0f : 0.0f);
    push.sky_zenith = glm::vec4(light.sky_zenith, 0.0f);
    push.sky_ground = glm::vec4(light.sky_ground, 0.0f);
    push.sun_color = glm::vec4(light.sun_color, light.sun_intensity);   // w: klx (ground-band lighting)
    push.sh = allocator_->get_buffer(sh_buffer_).device_address;

    // One-time: DFG LUT bake + move the cubemaps into their permanent GENERAL layout.
    if (!dfg_baked_)
    {
        const ::string::gpu::allocated_image& dfg = allocator_->get_image(dfg_lut_);
        String::vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .new_layout = VK_IMAGE_LAYOUT_GENERAL,
            .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
            .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        IblPush dpush = push;
        dpush.dst_slot = dfg_storage_slot_;
        dpush.dst_size = kDfgSize;
        dpush.sample_count = kDfgSamples;
        dispatch(dfg_program_, dpush, (kDfgSize + 7) / 8, (kDfgSize + 7) / 8, 1);
        String::vku::transition_image(cb, {
            .image = dfg.image, .old_layout = VK_IMAGE_LAYOUT_GENERAL,
            .new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .src_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        });
        dfg_baked_ = true;
    }
    if (!ibl_layouts_initialized_)
    {
        for (::string::gpu::resource_id id : { env_capture_, env_prefiltered_ })
        {
            const ::string::gpu::allocated_image& img = allocator_->get_image(id);
            String::vku::transition_image(cb, {
                .image = img.image, .old_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .new_layout = VK_IMAGE_LAYOUT_GENERAL,
                .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, .src_access = 0,
                .dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .dst_access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                            | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                .level_count = img.mip_levels,
                .layer_count = 6,
            });
        }
        ibl_layouts_initialized_ = true;
    }
    else
    {
        // Cross-frame WAR: last frame's fragment reads of the prefiltered ladder must retire
        // before this frame's rewrite (execution dependency; no memory flush needed for reads).
        const VkMemoryBarrier2 war = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        };
        const VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &war };
        vkCmdPipelineBarrier2(cb, &dep);
    }

    // 1) Sky -> capture mip 0.
    {
        IblPush cpush = push;
        cpush.dst_slot = env_capture_mip_slots_[0];
        cpush.dst_size = kEnvSize;
        dispatch(env_capture_program_, cpush, kEnvSize / 8, kEnvSize / 8, 6);
    }
    // 2) Capture average chain (PDF-mip source + SH source).
    for (uint32_t m = 1; m < kEnvCaptureMips; ++m)
    {
        compute_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        IblPush mpush = push;
        mpush.src_slot = env_capture_mip_slots_[m - 1];
        mpush.dst_slot = env_capture_mip_slots_[m];
        mpush.src_size = kEnvSize >> (m - 1);
        mpush.dst_size = kEnvSize >> m;
        dispatch(env_mip_program_, mpush, (mpush.dst_size + 7) / 8, (mpush.dst_size + 7) / 8, 6);
    }
    // Capture writes -> SH storage reads + prefilter SAMPLED reads.
    compute_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    // 3) L2 SH projection (one workgroup; the fragment-read edge is graph-declared in usages).
    {
        IblPush spush = push;
        spush.src_slot = env_capture_mip_slots_[kShSourceMip];
        spush.src_size = kEnvSize >> kShSourceMip;
        dispatch(sh_project_program_, spush, 1, 1, 1);
    }
    // 4) GGX prefilter ladder (mips independent — no barriers between them).
    for (uint32_t m = 0; m < kEnvPrefilterMips; ++m)
    {
        IblPush ppush = push;
        ppush.src_slot = env_capture_sample_slot_;
        ppush.src_size = kEnvSize;
        ppush.dst_slot = env_prefiltered_mip_slots_[m];
        ppush.dst_size = kEnvSize >> m;
        ppush.roughness = float(m) / float(kEnvPrefilterMips - 1);
        ppush.sample_count = kPrefilterSamples;
        ppush.mip_count = kEnvCaptureMips;
        dispatch(env_prefilter_program_, ppush, (ppush.dst_size + 7) / 8, (ppush.dst_size + 7) / 8, 6);
    }
    // Ladder writes -> the lit fragments' sampled reads (image stays in GENERAL).
    compute_barrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    ibl_captured_sun_dir_ = glm::normalize(light.sun_dir);
    ibl_captured_furnace_ = light.furnace;
    ibl_primed_ = true;
    ibl_update_pending_ = false;   // (GeometryPass cleared this after the call before brief 11)
    ++ibl_update_count_;
}

VkDeviceAddress IblComponent::sh_address() const
{
    return sh_buffer_ != 0 ? allocator_->get_buffer(sh_buffer_).device_address : 0;
}

void IblComponent::destroy()
{
    if (!device_) return;
    const auto destroy_program = [this](::string::gpu::shader_program* prog) {
        if (!prog) return;
        const ::string::gpu::pipeline& p = prog->current();
        vkDestroyPipeline(device_->get_device(), p.pipeline, nullptr);
        vkDestroyPipelineLayout(device_->get_device(), p.pipeline_layout, nullptr);
    };
    destroy_program(env_capture_program_);
    destroy_program(env_mip_program_);
    destroy_program(env_prefilter_program_);
    destroy_program(sh_project_program_);
    destroy_program(dfg_program_);

    if (env_capture_ != 0)
    {
        const auto drop_cube = [&](::string::gpu::resource_id id, std::vector<VkImageView>& views,
                                   std::vector<uint32_t>& slots) {
            for (uint32_t s : slots) table_->unbind_storage_view(s);
            for (VkImageView v : views) vkDestroyImageView(device_->get_device(), v, nullptr);
            table_->unbind(id, ::string::gpu::descriptor_type::TEXTURE);
            allocator_->destroy_resource(id);
        };
        drop_cube(env_capture_, env_capture_mip_views_, env_capture_mip_slots_);
        drop_cube(env_prefiltered_, env_prefiltered_mip_views_, env_prefiltered_mip_slots_);
        table_->unbind_storage_view(dfg_storage_slot_);
        table_->unbind(dfg_lut_, ::string::gpu::descriptor_type::TEXTURE);
        allocator_->destroy_resource(dfg_lut_);
        allocator_->destroy_resource(sh_buffer_);
        vkDestroySampler(device_->get_device(), env_sampler_, nullptr);
    }
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

void IblComponent::run_verification(bool furnace)
{
    if (sh_buffer_ == 0 || dfg_lut_ == 0) return;
    vkDeviceWaitIdle(device_->get_device());

    const VkDeviceSize sh_bytes = sizeof(float) * 4 * 9;
    const VkDeviceSize dfg_bytes = VkDeviceSize(kDfgSize) * kDfgSize * 8;   // RGBA16F
    const ::string::gpu::resource_id staging = allocator_->create_resource(::string::gpu::buffer_info{
        .size = sh_bytes + dfg_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_TO_CPU,
        .allocation_flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                          | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
    });
    {
        ::string::gpu::command_recorder rec;
        rec.init(device_->get_device(), device_->get_queue(::string::gpu::queue_type::GRAPHICS));
        VkCommandBuffer cb = rec.begin();
        const VkBufferCopy sh_region = { 0, 0, sh_bytes };
        vkCmdCopyBuffer(cb, allocator_->get_buffer(sh_buffer_).buffer,
                        allocator_->get_buffer(staging).buffer, 1, &sh_region);
        const ::string::gpu::allocated_image& dfg = allocator_->get_image(dfg_lut_);
        String::vku::transition_image(cb, {
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
                               allocator_->get_buffer(staging).buffer, 1, &dfg_region);
        String::vku::transition_image(cb, {
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
        allocator_->get_buffer(staging).allocation_info.pMappedData);
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
    allocator_->destroy_resource(staging);
}

}  // namespace string::render
