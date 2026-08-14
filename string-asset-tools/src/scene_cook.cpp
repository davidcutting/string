#include <string/asset/tools/scene_cook.hpp>

#include <chrono>

#include <string/core/logger.hpp>

#include <string/asset/cooked_scene.hpp>
#include <string/asset/manifest.hpp>
#include <string/asset/tools/bake.hpp>
#include <string/asset/tools/texture_cook.hpp>

namespace string::asset::tools
{

TextureCookSummary cook_scene_textures_for(const std::filesystem::path& resources_path,
                                           const std::vector<std::filesystem::path>& model_paths,
                                           uint32_t chunk_budget)
{
    const auto t0 = std::chrono::steady_clock::now();
    TextureCookSummary out;

    for (const std::filesystem::path& rel : model_paths)
    {
        const std::filesystem::path src = resources_path / rel;
        const std::filesystem::path cooked_path = cooked_path_for(src, chunk_budget);

        // The texture list (with each image's srgb flag, taken from its glTF material role) lives in
        // the cooked scene, so cook the geometry first if it is missing — that is the cheap half and
        // it is what the load path would do anyway.
        CookedScene scene;
        if (needs_recook(src, cooked_path, chunk_budget) || !read_cooked_file(cooked_path, scene))
        {
            STRING_LOG_INFO("[cook] baking geometry first: {}", src.string());
            BakeParams params;
            params.chunk_max_meshlets = chunk_budget;
            try
            {
                scene = bake_gltf(src, params);
                write_cooked_file(scene, cooked_path);
                update_manifest(src, cooked_path, chunk_budget, scene.source_content_hash);
            }
            catch (const std::exception& e)
            {
                STRING_LOG_WARN("[cook] {}: {}", src.string(), e.what());
                ++out.failed;
                continue;
            }
        }

        STRING_LOG_INFO("[cook] {} textures for {} (BC7 — this takes a while)",
                        scene.textures.size(), src.filename().string());
        const TextureCookStats stats =
            cook_scene_textures(scene, src.parent_path(), TextureCookParams{});
        out.cooked += stats.cooked;
        out.skipped += stats.skipped;
        out.failed += stats.failed;
    }

    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    STRING_LOG_INFO("[cook] textures done: {} cooked, {} already fresh, {} failed in {:.1f} s",
                    out.cooked, out.skipped, out.failed, out.ms / 1000.0);
    return out;
}

}  // namespace string::asset::tools
