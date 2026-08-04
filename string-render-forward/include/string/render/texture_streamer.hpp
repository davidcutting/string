#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <unordered_map>
#include <vector>

#include <string/core/job_system.hpp>
#include <string/gpu/descriptor_allocator.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/residency_manager.hpp>
#include <string/gpu/resource.hpp>
#include <string/gpu/resource_allocator.hpp>
#include <string/vulkan/transfer_batch.hpp>

#include <volk.h>

// libktx: the transcoded BC7 blob per texture (mip levels we stream from).
#include <ktx.h>

namespace string::render
{

// Streams a glTF model's KTX2 textures, as a residency_provider driving the engine's
// residency_manager. The cooked files hold BC7 already (baked by string-asset-tools' texture cook,
// which `string_cook` runs alongside the geometry bake), so streaming a
// mip is a plain read + upload with no transcode: the dominant ~1.5 GB UASTC->BC7 transcode has
// moved offline. libktx still opens the file on a background worker (it inflates the zstd payload,
// I/O + decompress worth keeping off the render thread), and a UASTC->BC7 transcode fallback is
// kept for any .ktx2 that isn't already BC7 — but for cooked assets nothing is transcoded.
//
// At load a texture's BC7 image is created at full mip count (only partially populated) and its
// bindless slot points at a shared placeholder (a flat white texture, tinted by the material's base
// color). The residency_manager then drives detail up: it first streams a coarse tail (the coarsest
// few mips — tiny) so the whole scene renders blurry-but-real within a frame or two, then streams
// finer mips into the visible textures on demand (screen-coverage feedback in GeometryPass).
//
// "detail" (the residency_manager's currency) is the number of resident mip levels counted from the
// coarsest, so detail == mip_levels means every level is resident (finest base mip 0) and detail 0
// means the placeholder. base_mip = mip_levels - detail = the sampler's minLod.
//
// Memory: no blob is cached. A stream loads the whole BC7 file, records the mip uploads it needs
// (the TransferBatch copies the bytes into staging immediately), then frees the file — so RAM holds
// only the blobs of streams currently in flight, not every texture. A later stream to finer detail
// re-reads the file (cheap: a zstd inflate, or a transcode on the fallback path). `phys_base` tracks
// the finest mip already physically uploaded, so re-wanting detail after a *logical* eviction (v1
// eviction is minLod-only, no VRAM reclaim) uploads nothing.
class TextureStreamer final : public ::string::gpu::residency_provider
{
public:
    TextureStreamer(::string::gpu::device& device, ::string::gpu::resource_allocator& allocator,
                    ::string::gpu::descriptor_table& descriptor_table, String::TransferBatch& transfer,
                    VkImageView placeholder_view, VkSampler placeholder_sampler);
    ~TextureStreamer() override;

    TextureStreamer(const TextureStreamer&) = delete;
    TextureStreamer& operator=(const TextureStreamer&) = delete;

    struct Registered
    {
        ::string::gpu::resource_id image;
        std::uint32_t slot;         // bindless texture slot (stable for the image's lifetime)
        std::uint32_t min_detail;   // 0 (placeholder is the pinned floor)
        std::uint32_t max_detail;   // finest (all levels)
        std::uint32_t base_extent;  // max(width, height) of mip 0 — for the caller's LOD heuristic
    };
    // Register a cooked KTX2 by path (data load deferred to the first stream). `srgb` selects the
    // BC7 format variant when the file must be transcoded; an already-BC7 file's own format wins.
    // Reads only the header (dimensions + level count) to create the image; returns the handle to
    // register with the residency_manager.
    Registered add(const std::filesystem::path& path, bool srgb);

    // residency_provider (note: the returned/queried value is an internal token, not a transfer
    // ticket — a stream spans an async transcode phase then a GPU upload phase):
    std::uint64_t stream(::string::gpu::resource_id id, std::uint32_t from_detail, std::uint32_t to_detail) override;
    bool is_complete(std::uint64_t token) override;
    void on_resident(::string::gpu::resource_id id, std::uint32_t detail) override;
    void evict(::string::gpu::resource_id id, std::uint32_t from_detail, std::uint32_t to_detail) override;
    VkDeviceSize cost(::string::gpu::resource_id id, std::uint32_t detail) override;

    // Diagnostics.
    std::size_t count() const { return textures_.size(); }

private:
    struct Texture
    {
        std::filesystem::path path;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::uint32_t levels = 0;    // total mip levels (== max_detail)
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        ::string::gpu::resource_id image = 0;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t slot = 0;

        // Streaming state. phys_base = finest mip index physically uploaded so far (levels = none).
        std::uint32_t phys_base = 0;
        bool loading = false;             // a background file load is in flight
        std::future<ktxTexture2*> future;
        std::uint32_t pending_base = 0;   // finest mip index the in-flight load must upload down to
        std::uint64_t gpu_ticket = 0;
    };

    // Record the uploads for mip levels [first_level, last_level] from an opened BC7 blob, advancing
    // phys_base. Returns the last upload's transfer ticket.
    std::uint64_t upload_levels(Texture& t, ktxTexture2* ktx, std::uint32_t first_level,
                                std::uint32_t last_level);
    static std::uint32_t base_of(const Texture& t, std::uint32_t detail) { return t.levels - detail; }
    VkSampler sampler_for(std::uint32_t base);
    // Once a texture's load future is ready: collect the blob, record the uploads it needs (down to
    // pending_base), free the blob, and set gpu_ticket. Returns true if the blob is ready.
    bool finish_load(Texture& t);

    ::string::gpu::device& device_;
    ::string::gpu::resource_allocator& allocator_;
    ::string::gpu::descriptor_table& descriptor_table_;
    String::TransferBatch& transfer_;
    VkImageView placeholder_view_;
    VkSampler placeholder_sampler_;
    float max_anisotropy_ = 1.0f;

    // Background file-load workers (zstd inflate, and a UASTC->BC7 transcode on the fallback path;
    // keep the I/O + CPU off the render thread).
    ::string::core::job_system load_pool_;

    std::unordered_map<::string::gpu::resource_id, Texture> textures_;
    std::unordered_map<std::uint32_t, VkSampler> samplers_;      // keyed by minLod base mip
    std::unordered_map<std::uint64_t, ::string::gpu::resource_id> token_to_id_;
    std::uint64_t next_token_ = 1;
};

}  // namespace string::render
