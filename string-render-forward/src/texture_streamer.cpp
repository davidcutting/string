#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>

#include <string/render/texture_streamer.hpp>

#include <string/core/logger.hpp>

#include <ktxvulkan.h>

namespace string::render
{
using namespace String;

namespace
{

// CPU work (safe on a job thread): open a cooked .ktx2 and its image data (inflating the zstd
// payload). Cooked files are already BC7, so this is just a load; a UASTC->BC7 transcode is only
// done as a fallback for a file that still needs it (an un-baked UASTC .ktx2). Returns an owning
// ktxTexture2* (freed by the streamer). Throws on failure (rethrown at the future's .get()).
ktxTexture2* load_ktx2_bc7(const std::filesystem::path& path)
{
    ktxTexture2* ktx = nullptr;
    KTX_error_code rc = ktxTexture2_CreateFromNamedFile(
        path.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);
    if (rc != KTX_SUCCESS)
    {
        throw std::runtime_error("ktx: failed to load " + path.string() + ": " + ktxErrorString(rc));
    }
    if (ktxTexture2_NeedsTranscoding(ktx))
    {
        // Fallback: not a baked-BC7 file. Transcode UASTC->BC7 (the old runtime cost) so the
        // uncooked asset still loads.
        rc = ktxTexture2_TranscodeBasis(ktx, KTX_TTF_BC7_RGBA, 0);
        if (rc != KTX_SUCCESS)
        {
            ktxTexture_Destroy(ktxTexture(ktx));
            throw std::runtime_error("ktx: BC7 transcode failed for " + path.string() + ": " +
                                     ktxErrorString(rc));
        }
    }
    return ktx;
}

}  // namespace

TextureStreamer::TextureStreamer(::string::gpu::device& device, ::string::gpu::resource_allocator& allocator,
                                 ::string::gpu::descriptor_table& descriptor_table, TransferBatch& transfer,
                                 VkImageView placeholder_view, VkSampler placeholder_sampler)
: device_(device)
, allocator_(allocator)
, descriptor_table_(descriptor_table)
, transfer_(transfer)
, placeholder_view_(placeholder_view)
, placeholder_sampler_(placeholder_sampler)
, max_anisotropy_(device.get_physical_device_limits().maxSamplerAnisotropy)
{
}

TextureStreamer::~TextureStreamer()
{
    for (auto& [id, t] : textures_)
    {
        // A load may still be in flight (app closed mid-stream); drain it so its ktx isn't leaked
        // and the future doesn't outlive the pool. Streams free the blob as soon as they've staged
        // their uploads, so the blob itself needs nothing else.
        if (t.future.valid())
        {
            try { ktxTexture2* k = t.future.get(); if (k) ktxTexture_Destroy(ktxTexture(k)); }
            catch (...) {}
        }

        // The GPU side of each streamed texture: bindless slot, view, image. This used to be left to
        // the device teardown, which was harmless while a streamer only ever died with the process.
        // It stops being harmless the moment a scene can be UNLOADED while the app keeps running —
        // Sponza's texture set would leak its VRAM and its bindless slots on every switch away.
        // `t.view` is a CACHED COPY of the allocator's own view (see stream-in), not a second view
        // this class created — so destroying it here as well as in destroy_resource() is a
        // double-free that validation catches as VUID-vkDestroyImageView-imageView-parameter.
        // The allocator owns the image, its view and its sampler; this only has to give back the
        // bindless slot it took.
        if (t.image != 0)
        {
            descriptor_table_.unbind(t.image, ::string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(t.image);
        }
    }
    for (auto& [base, sampler] : samplers_)
    {
        vkDestroySampler(device_.get_device(), sampler, nullptr);
    }
}

VkSampler TextureStreamer::sampler_for(std::uint32_t base)
{
    auto it = samplers_.find(base);
    if (it != samplers_.end())
    {
        return it->second;
    }
    const VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mipLodBias = 0.f,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = max_anisotropy_,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = static_cast<float>(base),
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device_.get_device(), &info, nullptr, &sampler) != VK_SUCCESS)
    {
        throw std::runtime_error("TextureStreamer: failed to create minLod sampler");
    }
    samplers_[base] = sampler;
    return sampler;
}

std::uint64_t TextureStreamer::upload_levels(Texture& t, ktxTexture2* ktx, std::uint32_t first_level,
                                             std::uint32_t last_level)
{
    const ktx_uint8_t* blob = ktxTexture_GetData(ktxTexture(ktx));
    std::uint64_t ticket = 0;
    for (std::uint32_t lvl = first_level; lvl <= last_level; ++lvl)
    {
        ktx_size_t offset = 0;
        ktxTexture_GetImageOffset(ktxTexture(ktx), lvl, 0, 0, &offset);
        const VkDeviceSize size = ktxTexture_GetImageSize(ktxTexture(ktx), lvl);
        const VkExtent3D extent{ std::max(t.width >> lvl, 1u), std::max(t.height >> lvl, 1u), 1 };
        const TransferBatch::level_copy copy{ .offset = 0, .extent = extent };
        ticket = transfer_.upload_image_levels(blob + offset, size, { &copy, 1 }, t.image, lvl);
    }
    // These levels are now (recorded to be) physically on the GPU; the finest is first_level.
    t.phys_base = std::min(t.phys_base, first_level);
    return ticket;
}

TextureStreamer::Registered TextureStreamer::add(const std::filesystem::path& path, bool srgb)
{
    // Header-only read: numLevels + base extent, so we can create the image now without transcoding.
    ktxTexture2* header = nullptr;
    KTX_error_code rc = ktxTexture2_CreateFromNamedFile(path.string().c_str(), 0, &header);
    if (rc != KTX_SUCCESS)
    {
        throw std::runtime_error("ktx: failed to read header " + path.string() + ": " + ktxErrorString(rc));
    }
    Texture t;
    t.path = path;
    t.levels = header->numLevels;
    t.width = header->baseWidth;
    t.height = header->baseHeight;
    t.phys_base = t.levels;  // nothing physically uploaded yet
    const auto header_format = static_cast<VkFormat>(ktxTexture2_GetVkFormat(header));
    ktxTexture_Destroy(ktxTexture(header));

    // A baked-BC7 file's own format is authoritative (it already encodes the sRGB-ness the cook
    // chose). Only when the file isn't BC7 (an un-baked UASTC file we'll transcode) do we derive the
    // BC7 variant from the glTF usage the caller passed (base-color = sRGB, data maps = linear).
    if (header_format == VK_FORMAT_BC7_SRGB_BLOCK || header_format == VK_FORMAT_BC7_UNORM_BLOCK)
    {
        t.format = header_format;
    }
    else
    {
        t.format = srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    }

    const ::string::gpu::resource_id image = allocator_.create_resource(::string::gpu::image_info{
        .extent = { t.width, t.height, 1 },
        .format = t.format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .mip_levels = t.levels,
    });
    t.image = image;
    t.view = allocator_.get_image(image).view;

    // Allocate the bindless slot and point it at the placeholder until the real texture streams in.
    // (bind() writes the image's own — still UNDEFINED — view; immediately overwrite with the
    // placeholder so nothing samples the empty image.)
    descriptor_table_.bind(image, ::string::gpu::descriptor_type::TEXTURE);
    t.slot = descriptor_table_.get_binding_slot(image, ::string::gpu::descriptor_type::TEXTURE);
    // BRIEF 20 REGRESSION — see the brief's "texture streaming" note. update_texture is deleted, and
    // nothing replaces per-slot residency rebinding: the placeholder swap and the adjustable minLod
    // both needed it. bind() writes the image's OWN view and sampler, which is correct once a texture
    // is fully resident and wrong while it is streaming in.
    descriptor_table_.bind(image, ::string::gpu::descriptor_type::TEXTURE);

    const std::uint32_t slot = t.slot;
    const std::uint32_t levels = t.levels;
    const std::uint32_t base_extent = std::max(t.width, t.height);
    textures_[image] = std::move(t);
    return Registered{ .image = image, .slot = slot, .min_detail = 0, .max_detail = levels,
                       .base_extent = base_extent };
}

bool TextureStreamer::finish_load(Texture& t)
{
    if (t.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return false;
    }
    ktxTexture2* ktx = t.future.get();
    t.loading = false;

    if (static_cast<VkFormat>(ktxTexture2_GetVkFormat(ktx)) != t.format)
    {
        // The file's BC7 variant disagreed with the format the image was created with — sampling
        // will be off (sRGB vs linear). Shouldn't happen for cooked assets (add() reads the file's
        // own format), so this flags a bad cook / wrong glTF-usage assumption on the fallback path.
        STRING_LOG_WARN("TextureStreamer: BC7 format mismatch for {} (created {}, loaded {})",
                        t.path.string(), static_cast<int>(t.format),
                        static_cast<int>(ktxTexture2_GetVkFormat(ktx)));
    }

    // Record the uploads for the levels not yet physical: [pending_base, phys_base - 1].
    t.gpu_ticket = upload_levels(t, ktx, t.pending_base, t.phys_base - 1);
    // The bytes are now staged in the TransferBatch, so the blob is no longer needed — free it (no
    // RAM cache; a later finer stream re-reads the file).
    ktxTexture_Destroy(ktxTexture(ktx));
    return true;
}

std::uint64_t TextureStreamer::stream(::string::gpu::resource_id id, std::uint32_t /*from_detail*/,
                                      std::uint32_t to_detail)
{
    Texture& t = textures_.at(id);
    const std::uint64_t token = next_token_++;
    token_to_id_[token] = id;

    const std::uint32_t need_base = base_of(t, to_detail);  // finest mip index this detail needs
    if (need_base >= t.phys_base)
    {
        // Every level this detail needs is already physically uploaded (v1 eviction is minLod-only,
        // so physical mips survive). Nothing to upload — on_resident() just lowers minLod.
        t.gpu_ticket = 0;
        return token;
    }
    // The finer levels [need_base, phys_base - 1] aren't on the GPU yet: open the file on a worker
    // and record their uploads once it's loaded (is_complete drives finish_load). The manager keeps
    // this the only in-flight stream for the texture, so a plain load-then-upload is enough.
    t.pending_base = need_base;
    t.loading = true;
    t.gpu_ticket = 0;
    const std::filesystem::path path = t.path;
    t.future = load_pool_.enqueue([path]() { return load_ktx2_bc7(path); });
    return token;
}

bool TextureStreamer::is_complete(std::uint64_t token)
{
    auto it = token_to_id_.find(token);
    if (it == token_to_id_.end())
    {
        return true;
    }
    Texture& t = textures_.at(it->second);

    if (t.loading)
    {
        // Still loading — advance to the upload phase once the blob is ready, then keep waiting on
        // the GPU upload.
        if (!finish_load(t))
        {
            return false;
        }
    }
    const bool done = transfer_.is_complete(t.gpu_ticket);
    if (done)
    {
        token_to_id_.erase(it);
    }
    return done;
}

void TextureStreamer::on_resident(::string::gpu::resource_id id, std::uint32_t detail)
{
    Texture& t = textures_.at(id);
    if (detail == 0)
    {
        // BRIEF 20 REGRESSION — see the brief's "texture streaming" note. update_texture is deleted, and
    // nothing replaces per-slot residency rebinding: the placeholder swap and the adjustable minLod
    // both needed it. bind() writes the image's OWN view and sampler, which is correct once a texture
    // is fully resident and wrong while it is streaming in.
    descriptor_table_.bind(id, ::string::gpu::descriptor_type::TEXTURE);
        return;
    }
    // Swap the slot from the placeholder to the real image at the new minLod.
    descriptor_table_.bind(id, ::string::gpu::descriptor_type::TEXTURE);   // see note above
}

void TextureStreamer::evict(::string::gpu::resource_id id, std::uint32_t /*from_detail*/, std::uint32_t to_detail)
{
    // v1: logical only. Revert to the placeholder at detail 0, else raise minLod.
    Texture& t = textures_.at(id);
    if (to_detail == 0)
    {
        // BRIEF 20 REGRESSION — see the brief's "texture streaming" note. update_texture is deleted, and
    // nothing replaces per-slot residency rebinding: the placeholder swap and the adjustable minLod
    // both needed it. bind() writes the image's OWN view and sampler, which is correct once a texture
    // is fully resident and wrong while it is streaming in.
    descriptor_table_.bind(id, ::string::gpu::descriptor_type::TEXTURE);
    }
    else
    {
        descriptor_table_.bind(id, ::string::gpu::descriptor_type::TEXTURE);   // see note above
    }
}

VkDeviceSize TextureStreamer::cost(::string::gpu::resource_id id, std::uint32_t detail)
{
    Texture& t = textures_.at(id);
    if (detail == 0)
    {
        return 0;
    }
    // BC7 is a fixed 16 bytes per 4x4 block, so a level's GPU size is exact from its extent alone —
    // no need for the loaded blob. Sum the resident levels [base_of(detail), levels-1].
    VkDeviceSize bytes = 0;
    for (std::uint32_t lvl = base_of(t, detail); lvl < t.levels; ++lvl)
    {
        const VkDeviceSize w = std::max(t.width >> lvl, 1u);
        const VkDeviceSize h = std::max(t.height >> lvl, 1u);
        bytes += ((w + 3) / 4) * ((h + 3) / 4) * 16;
    }
    return bytes;
}

}  // namespace string::render
