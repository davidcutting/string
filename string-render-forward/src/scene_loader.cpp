#include <string/render/scene_loader.hpp>

#include <chrono>

#include <string/core/logger.hpp>

#include <string/asset/manifest.hpp>
#include <string/asset/tools/texture_cook.hpp>

namespace string::render
{

// The asset libraries' vocabulary, unqualified for the merge code below. This TU is the seam
// between them and the renderer: it reads cooked tables (::string::asset) and, on a cook-on-load
// miss, drives the importer (::string::asset::tools).
using namespace string::asset;
using namespace string::asset::tools;
}

namespace string::render
{
namespace
{

// Reconstruct a GltfMaterial from a cooked one (texture indices are still file-local here; the merge
// rebases them). The engine keeps GltfMaterial as its runtime material type unchanged.
GltfMaterial to_gltf_material(const CookedMaterial& c)
{
    GltfMaterial m;
    m.base_color_factor = c.base_color_factor;
    m.metallic_factor = c.metallic_factor;
    m.roughness_factor = c.roughness_factor;
    m.base_color_texture = c.base_color_texture;
    m.metallic_roughness_texture = c.metallic_roughness_texture;
    m.normal_texture = c.normal_texture;
    m.occlusion_texture = c.occlusion_texture;
    switch (static_cast<CookedAlphaMode>(c.alpha_mode))
    {
    case CookedAlphaMode::Mask:  m.alpha_mode = GltfAlphaMode::Mask;  break;
    case CookedAlphaMode::Blend: m.alpha_mode = GltfAlphaMode::Blend; break;
    default:                     m.alpha_mode = GltfAlphaMode::Opaque; break;
    }
    m.alpha_cutoff = c.alpha_cutoff;
    m.double_sided = c.double_sided != 0;
    return m;
}

// Reconstruct a GltfTexture reference from a cooked one. `path` is relative to the source glTF's
// directory; resolve it to an absolute path so ktx_sibling() + the stb fallback find the file (same
// resolution the old loader did: base_dir / uri).
GltfTexture to_gltf_texture(const CookedTexture& c, const std::filesystem::path& base_dir)
{
    GltfTexture t;
    t.srgb = c.srgb != 0;
    if (c.path[0] != '\0')
        t.file = base_dir / std::filesystem::path(std::string(c.path));
    // Embedded textures (empty path) leave t.file empty -> the pass uses the white fallback (Sponza
    // has none; brief 04b keeps textures as-is).
    return t;
}

}  // namespace

LoadedScene load_cooked_scenes(const std::filesystem::path& resources_path,
                               const std::vector<std::filesystem::path>& model_paths,
                               uint32_t chunk_budget)
{
    const auto t0 = std::chrono::steady_clock::now();
    LoadedScene out;

    for (const std::filesystem::path& rel : model_paths)
    {
        const std::filesystem::path src = resources_path / rel;
        const std::filesystem::path cooked_path = cooked_path_for(src, chunk_budget);

        CookedScene scene;
        bool ok = false;
        if (!needs_recook(src, cooked_path, chunk_budget))
        {
            ok = read_cooked_file(cooked_path, scene);
            if (ok) ++out.cooked_hits;
            else
                STRING_LOG_WARN("[load] cooked file unreadable, re-cooking: {}", cooked_path.string());
        }
        if (!ok)
        {
            // Missing / stale / unreadable: cook in-process via the bake library and write it next to
            // the source (cooked outputs are gitignored, like sponza). Log the CLI hint.
            STRING_LOG_WARN("[load] cooking in-process (missing/stale cooked scene): {} "
                            "-- run `nix run .#cook -- {}` to precook",
                            src.string(), src.string());
            BakeParams params;
            params.chunk_max_meshlets = chunk_budget;
            scene = bake_gltf(src, params);
            try
            {
                write_cooked_file(scene, cooked_path);
                update_manifest(src, cooked_path, chunk_budget, scene.source_content_hash);
            }
            catch (const std::exception& e)
            {
                STRING_LOG_WARN("[load] could not write cooked file {}: {}", cooked_path.string(), e.what());
            }
            ++out.cooked_misses;
        }

        // --- Merge this cooked scene into the running tables (same rebasing as the old loader, plus
        // the meshlet-heap rebasing the old path got for free by meshletizing after the vertex merge).
        const uint32_t vertex_base   = static_cast<uint32_t>(out.vertices.size());
        const uint32_t meshlet_base  = static_cast<uint32_t>(out.meshlets.size());
        const uint32_t mvert_base    = static_cast<uint32_t>(out.meshlet_vertices.size());
        const uint32_t mtri_base     = static_cast<uint32_t>(out.meshlet_triangles.size());
        const int32_t  tex_base      = static_cast<int32_t>(out.textures.size());
        const int32_t  material_base = static_cast<int32_t>(out.materials.size());

        const std::filesystem::path base_dir = src.parent_path();

        // Vertices: append (heap grows; meshlet-vertex remap entries rebase by vertex_base below).
        out.vertices.insert(out.vertices.end(), scene.vertices.begin(), scene.vertices.end());

        // Meshlet-vertex remap: hold FILE-LOCAL global vertex indices -> rebase to the merged heap.
        out.meshlet_vertices.reserve(out.meshlet_vertices.size() + scene.meshlet_vertices.size());
        for (uint32_t v : scene.meshlet_vertices) out.meshlet_vertices.push_back(v + vertex_base);

        // Triangle words: local 8-bit indices, no rebasing (they index within a meshlet's vertex run).
        out.meshlet_triangles.insert(out.meshlet_triangles.end(),
                                     scene.meshlet_triangles.begin(), scene.meshlet_triangles.end());

        // Meshlets: their vertex_offset/triangle_offset index the merged heaps -> rebase by heap bases.
        out.meshlets.reserve(out.meshlets.size() + scene.meshlets.size());
        for (GpuMeshlet m : scene.meshlets)
        {
            m.vertex_offset += mvert_base;
            m.triangle_offset += mtri_base;
            out.meshlets.push_back(m);
        }

        // Materials: rebase texture indices by this file's texture base.
        for (const CookedMaterial& cm : scene.materials)
        {
            GltfMaterial m = to_gltf_material(cm);
            const auto rebase = [tex_base](int32_t& idx) { if (idx >= 0) idx += tex_base; };
            rebase(m.base_color_texture);
            rebase(m.metallic_roughness_texture);
            rebase(m.normal_texture);
            rebase(m.occlusion_texture);
            out.materials.push_back(m);
        }

        // Textures: resolve relative paths against this file's directory.
        for (const CookedTexture& ct : scene.textures)
            out.textures.push_back(to_gltf_texture(ct, base_dir));

        // Draws: build the geometry-only GpuDrawInfo (LOD ranges + bounds, rebased into the merged
        // meshlet heap + vertex window) + the GltfDraw meta the pass keeps for culling/streaming.
        for (const CookedDraw& cd : scene.draws)
        {
            GpuDrawInfo info{};
            info.center = cd.center;
            info.radius = cd.radius;
            info.lod_count = cd.lod_count;
            info.first_meshlet = cd.first_meshlet + meshlet_base;
            info.total_meshlets = cd.total_meshlets;
            for (uint32_t l = 0; l < kMaxLods; ++l)
            {
                info.lods[l] = cd.lods[l];
                if (l < cd.lod_count) info.lods[l].meshlet_offset += meshlet_base;
            }
            out.draws.push_back(info);

            LoadedScene::VertexWindow win;
            win.offset = cd.vertex_count == 0 ? 0u : cd.vertex_offset + vertex_base;
            win.count = cd.vertex_count;
            out.draw_windows.push_back(win);

            GltfDraw meta;
            meta.index_offset = cd.index_offset;   // informational (streamer uses the window now)
            meta.index_count = cd.index_count;
            meta.material = cd.material >= 0 ? cd.material + material_base : -1;
            meta.transform = cd.transform;
            meta.aabb_min = cd.aabb_min;
            meta.aabb_max = cd.aabb_max;
            out.draws_meta.push_back(meta);
        }
    }

    out.total_meshlets = static_cast<uint32_t>(out.meshlets.size());
    out.load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

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

}  // namespace string::render
