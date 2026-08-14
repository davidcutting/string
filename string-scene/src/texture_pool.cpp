#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include <string/scene/texture_pool.hpp>

#include <string/core/logger.hpp>

#include <ktxvulkan.h>

namespace string::assets
{
using namespace string;

namespace
{

// CPU work (safe on a job thread): open a cooked .ktx2 and its image data (inflating the zstd
// payload). Cooked files are already BC7, so this is just a load; a UASTC->BC7 transcode is only
// done as a fallback for a file that still needs it (an un-baked UASTC .ktx2). Returns an owning
// ktxTexture2* (freed by the pool). Throws on failure (rethrown at the future's .get()).
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

texture_pool::texture_pool(::string::gpu::resource_allocator& allocator,
                           ::string::gpu::descriptor_table& descriptor_table, TransferBatch& transfer)
: allocator_(allocator)
, descriptor_table_(descriptor_table)
, transfer_(transfer)
{
    // 1x1 white fallback for materials without a base-color texture (the factor still tints it).
    const std::array<uint8_t, 4> white_pixel = { 255, 255, 255, 255 };
    white_image_ = allocator_.create_resource(::string::gpu::image_info{
        .extent = { 1, 1, 1 },
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    transfer_.upload_image(white_pixel.data(), white_pixel.size(), white_image_);
    descriptor_table_.bind(white_image_, ::string::gpu::descriptor_type::TEXTURE);
    white_slot_ =
        descriptor_table_.get_binding_slot(white_image_, ::string::gpu::descriptor_type::TEXTURE);

    // 1x1 flat-normal fallback (tangent-space +Z): materials with no normal map sample this and get
    // the geometric normal back, so the fragment shader never branches on "has a normal map".
    const std::array<uint8_t, 4> flat_normal_pixel = { 128, 128, 255, 255 };
    flat_normal_image_ = allocator_.create_resource(::string::gpu::image_info{
        .extent = { 1, 1, 1 },
        .format = VK_FORMAT_R8G8B8A8_UNORM,   // linear, not sRGB — it's data, not colour
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
    });
    transfer_.upload_image(flat_normal_pixel.data(), flat_normal_pixel.size(), flat_normal_image_);
    descriptor_table_.bind(flat_normal_image_, ::string::gpu::descriptor_type::TEXTURE);
    flat_normal_slot_ = descriptor_table_.get_binding_slot(flat_normal_image_,
                                                          ::string::gpu::descriptor_type::TEXTURE);
}

texture_pool::~texture_pool()
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

        // The GPU side of each streamed texture: bindless slot, view, image. `t.view` is a CACHED
        // COPY of the allocator's own view (see add), not a second view — the allocator owns the
        // image, its view and its sampler; this only has to give back the bindless slot it took.
        if (t.image != 0)
        {
            descriptor_table_.unbind(t.image, ::string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(t.image);
        }
    }
    // The whole-uploaded set + the two fallbacks: same ownership rule, one place.
    for (const ::string::gpu::resource_id id : uploaded_)
    {
        descriptor_table_.unbind(id, ::string::gpu::descriptor_type::TEXTURE);
        allocator_.destroy_resource(id);
    }
    for (const ::string::gpu::resource_id id : { white_image_, flat_normal_image_ })
    {
        if (id != 0)
        {
            descriptor_table_.unbind(id, ::string::gpu::descriptor_type::TEXTURE);
            allocator_.destroy_resource(id);
        }
    }
}

texture_pool::Uploaded texture_pool::add_decoded(const decoded_image& decoded)
{
    // Full mip chain: floor(log2(max dimension)) + 1 levels. TRANSFER_SRC is needed too because
    // mip generation blits from each level down to the next.
    const uint32_t max_dim = static_cast<uint32_t>(std::max(decoded.width, decoded.height));
    const uint32_t mip_levels = static_cast<uint32_t>(std::floor(std::log2(max_dim))) + 1;

    const ::string::gpu::resource_id image = allocator_.create_resource(::string::gpu::image_info{
        .extent = { static_cast<uint32_t>(decoded.width), static_cast<uint32_t>(decoded.height), 1 },
        .format = decoded.format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
        .memory_usage = VMA_MEMORY_USAGE_GPU_ONLY,
        .allocation_flags = {},
        .mip_levels = mip_levels,
    });
    // upload_image copies the pixels into staging immediately; the batch streams it to the GPU
    // asynchronously (bounded by its staging budget), so no per-texture stall.
    const VkDeviceSize size = static_cast<VkDeviceSize>(decoded.width) * decoded.height * 4;
    transfer_.upload_image(decoded.pixels.get(), size, image);

    descriptor_table_.bind(image, ::string::gpu::descriptor_type::TEXTURE);
    const uint32_t slot =
        descriptor_table_.get_binding_slot(image, ::string::gpu::descriptor_type::TEXTURE);
    uploaded_.push_back(image);
    return Uploaded{ .image = image, .slot = slot };
}

std::uint64_t texture_pool::upload_levels(Texture& t, ktxTexture2* ktx, std::uint32_t first_level,
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

texture_pool::Registered texture_pool::add(const std::filesystem::path& path, bool srgb)
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
        // Nothing is uploaded yet, so pin sampling to the coarsest level — the one the very first
        // stream (the coarse tail) fills. rebind() lowers this as finer levels land.
        .sampler = { .min_lod = static_cast<float>(t.levels - 1) },
        .mip_levels = t.levels,
    });
    t.image = image;
    t.view = allocator_.get_image(image).view;

    // Allocate the bindless slot. bind() writes the image's own view and its current sampler, so the
    // minLod above is what the slot carries until the first on_resident.
    descriptor_table_.bind(image, ::string::gpu::descriptor_type::TEXTURE);
    t.slot = descriptor_table_.get_binding_slot(image, ::string::gpu::descriptor_type::TEXTURE);

    const std::uint32_t slot = t.slot;
    const std::uint32_t levels = t.levels;
    const std::uint32_t base_extent = std::max(t.width, t.height);
    textures_[image] = std::move(t);
    return Registered{ .image = image, .slot = slot, .min_detail = 0, .max_detail = levels,
                       .base_extent = base_extent };
}

bool texture_pool::finish_load(Texture& t)
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
        STRING_LOG_WARN("texture_pool: BC7 format mismatch for {} (created {}, loaded {})",
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

std::uint64_t texture_pool::stream(::string::gpu::resource_id id, std::uint32_t /*from_detail*/,
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

bool texture_pool::is_complete(std::uint64_t token)
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

// Point the slot at the mip range that is actually on the GPU. `phys_base` is the truth (the finest
// level physically uploaded); `detail` is the residency manager's currency and agrees with it once a
// stream completes, so the coarser of the two is always the safe floor.
//
// This is the residency rebinding brief 20 left without a verb (brief 21's G8): the sampler is a
// property of the image now, so raising detail is a sampler swap plus a rebind, not a descriptor
// write from outside.
void texture_pool::rebind(Texture& t, std::uint32_t detail)
{
    const std::uint32_t resident_base = std::max(base_of(t, detail), t.phys_base);
    const float min_lod = static_cast<float>(std::min(resident_base, t.levels - 1));
    allocator_.set_sampler(t.image, ::string::gpu::sampler_info{ .min_lod = min_lod });
    descriptor_table_.bind(t.image, ::string::gpu::descriptor_type::TEXTURE);

    // STRING_STREAM_LOG=1: what each texture's slot is actually allowed to sample. A texture stuck at
    // a coarse minLod is a residency-feedback problem; one whose minLod is finer than phys_base would
    // be an upload-ordering problem. Both used to be invisible.
    static const bool log_stream = std::getenv("STRING_STREAM_LOG") != nullptr;
    if (log_stream)
    {
        STRING_LOG_INFO("[stream] slot {} ({}): detail {}/{} phys_base {} -> minLod {}", t.slot,
                        t.path.filename().string(), detail, t.levels, t.phys_base, min_lod);
    }
}

void texture_pool::on_resident(::string::gpu::resource_id id, std::uint32_t detail)
{
    rebind(textures_.at(id), detail);
}

void texture_pool::evict(::string::gpu::resource_id id, std::uint32_t /*from_detail*/, std::uint32_t to_detail)
{
    // v1: logical only — no VRAM is reclaimed, so this just raises minLod back up. phys_base keeps
    // the physically-uploaded levels, which is why re-wanting the detail later uploads nothing.
    rebind(textures_.at(id), to_detail);
}

VkDeviceSize texture_pool::cost(::string::gpu::resource_id id, std::uint32_t detail)
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

}  // namespace string::assets
