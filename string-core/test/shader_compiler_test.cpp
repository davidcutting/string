#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include <string/gpu/shader_compiler.hpp>

using namespace string::gpu;

namespace
{

std::filesystem::path write_slang(const std::string& name, const std::string& src)
{
    const auto dir = std::filesystem::temp_directory_path() /
        ("slang_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream(path) << src;
    return path;
}

}  // namespace

// A vertex+fragment Slang program with a push constant compiles to SPIR-V, and reflection reports
// both stages plus the push-constant range.
TEST(ShaderCompiler, CompilesAndReflects)
{
    const std::string src = R"(
        struct Push { uint slot; float scale; };
        [[vk::push_constant]] Push pc;

        struct VSOut { float4 pos : SV_Position; };

        [shader("vertex")]
        VSOut vertex_main(uint vid : SV_VertexID)
        {
            VSOut o;
            o.pos = float4(float(vid) * pc.scale, 0, 0, 1);
            return o;
        }

        [shader("fragment")]
        float4 fragment_main() : SV_Target { return float4(1, 1, 1, 1); }
    )";

    const auto path = write_slang("prog.slang", src);
    const auto cache = path.parent_path() / "cache";
    shader_compiler compiler(cache, { path.parent_path() });

    compile_error err;
    const auto program = compiler.compile(path, err);
    ASSERT_TRUE(program.has_value()) << err.message;

    ASSERT_EQ(program->stages.size(), 2u);
    bool has_vertex = false, has_fragment = false;
    for (const auto& s : program->stages)
    {
        EXPECT_FALSE(s.spirv.empty());
        if (s.stage == VK_SHADER_STAGE_VERTEX_BIT) has_vertex = true;
        if (s.stage == VK_SHADER_STAGE_FRAGMENT_BIT) has_fragment = true;
    }
    EXPECT_TRUE(has_vertex);
    EXPECT_TRUE(has_fragment);

    EXPECT_TRUE(program->layout.has_push_constant);
    EXPECT_GE(program->layout.push_constant.size, sizeof(uint32_t) + sizeof(float));

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

// A syntax error returns nullopt with a diagnostic, rather than throwing — the keep-last-good path.
TEST(ShaderCompiler, ReportsSyntaxError)
{
    const std::string src = R"(
        [shader("fragment")]
        float4 fragment_main() : SV_Target { return this is not valid slang; }
    )";
    const auto path = write_slang("broken.slang", src);
    const auto cache = path.parent_path() / "cache";
    shader_compiler compiler(cache, { path.parent_path() });

    compile_error err;
    const auto program = compiler.compile(path, err);
    EXPECT_FALSE(program.has_value());
    EXPECT_FALSE(err.message.empty());

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

// The disk cache is populated on first compile and reused on the second (same content hash).
TEST(ShaderCompiler, DiskCacheRoundTrips)
{
    const std::string src = R"(
        [shader("fragment")]
        float4 fragment_main() : SV_Target { return float4(0.5, 0.5, 0.5, 1); }
    )";
    const auto path = write_slang("cached.slang", src);
    const auto cache = path.parent_path() / "cache";
    shader_compiler compiler(cache, { path.parent_path() });

    compile_error err;
    const auto first = compiler.compile(path, err);
    ASSERT_TRUE(first.has_value()) << err.message;

    // A program cache file should now exist. It holds SPIR-V *and* reflection, so a hit needs no
    // Slang at all — hence ".program" rather than the older per-entry-point ".spv".
    bool cache_has_program = false;
    for (const auto& entry : std::filesystem::directory_iterator(cache))
    {
        if (entry.path().extension() == ".program") { cache_has_program = true; break; }
    }
    EXPECT_TRUE(cache_has_program);

    // Second compile returns identical SPIR-V (served from cache).
    const auto second = compiler.compile(path, err);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(first->stages.size(), second->stages.size());
    EXPECT_EQ(first->stages[0].spirv, second->stages[0].spirv);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

// The shipping guarantee: a cache written by one process must fully reconstruct the program in
// another, WITHOUT recompiling — entry-point names, stages and reflection included, not just SPIR-V.
// A fresh compiler over the same cache dir stands in for the second process.
TEST(ShaderCompiler, WarmCacheReconstructsWholeProgramInAFreshCompiler)
{
    const std::string src = R"(
        struct Push { float4 tint; };
        [vk::push_constant] Push pc;
        [shader("fragment")]
        float4 fragment_main() : SV_Target { return pc.tint; }
    )";
    const auto path = write_slang("warm.slang", src);
    const auto cache = path.parent_path() / "cache";

    compile_error err;
    std::optional<compiled_program> cold;
    {
        shader_compiler writer(cache, { path.parent_path() });
        cold = writer.compile(path, err);
        ASSERT_TRUE(cold.has_value()) << err.message;
    }

    shader_compiler reader(cache, { path.parent_path() });
    const auto warm = reader.compile(path, err);
    ASSERT_TRUE(warm.has_value()) << err.message;

    ASSERT_EQ(cold->stages.size(), warm->stages.size());
    EXPECT_EQ(cold->stages[0].entry_point, warm->stages[0].entry_point);
    EXPECT_EQ(cold->stages[0].stage, warm->stages[0].stage);
    EXPECT_EQ(cold->stages[0].spirv, warm->stages[0].spirv);

    // Reflection must survive the round trip, not just the code.
    EXPECT_EQ(cold->layout.has_push_constant, warm->layout.has_push_constant);
    EXPECT_TRUE(warm->layout.has_push_constant);
    EXPECT_EQ(cold->layout.push_constant.size, warm->layout.push_constant.size);
    EXPECT_EQ(cold->layout.push_constant.offset, warm->layout.push_constant.offset);
    EXPECT_EQ(cold->layout.push_constant.stageFlags, warm->layout.push_constant.stageFlags);
    ASSERT_EQ(cold->layout.sets.size(), warm->layout.sets.size());
    for (size_t s = 0; s < cold->layout.sets.size(); ++s)
    {
        ASSERT_EQ(cold->layout.sets[s].size(), warm->layout.sets[s].size());
        for (size_t b = 0; b < cold->layout.sets[s].size(); ++b)
        {
            EXPECT_EQ(cold->layout.sets[s][b].binding, warm->layout.sets[s][b].binding);
            EXPECT_EQ(cold->layout.sets[s][b].descriptorType, warm->layout.sets[s][b].descriptorType);
            EXPECT_EQ(cold->layout.sets[s][b].descriptorCount, warm->layout.sets[s][b].descriptorCount);
            EXPECT_EQ(cold->layout.sets[s][b].stageFlags, warm->layout.sets[s][b].stageFlags);
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

// A cache baked at package time must still hit after the user installs somewhere else. The key
// folds in the import closure, which used to include each module's ABSOLUTE path — so moving the
// tree changed every key and a shipped cache missed 100%. Regression test for that: same files,
// different directory, same cache must still serve.
TEST(ShaderCompiler, CacheKeyIsIndependentOfInstallLocation)
{
    const std::string src = R"(
        [shader("fragment")]
        float4 fragment_main() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }
    )";
    const auto path_a = write_slang("located.slang", src);
    const auto root_a = path_a.parent_path();
    const auto shared_cache = root_a.parent_path() / "shared_cache";

    compile_error err;
    shader_compiler at_a(shared_cache, { root_a });
    const auto from_a = at_a.compile(path_a, err);
    ASSERT_TRUE(from_a.has_value()) << err.message;

    // Move the whole shader tree somewhere else, leaving the cache where it is.
    const auto root_b = root_a.parent_path() / "moved_elsewhere";
    std::error_code ec;
    std::filesystem::rename(root_a, root_b, ec);
    ASSERT_FALSE(ec) << ec.message();

    // Read-only prebuilt slot, and a FRESH writable dir, so a hit can only come from the old cache.
    const auto empty_writable = root_b.parent_path() / "empty_cache";
    shader_compiler at_b(empty_writable, { root_b }, shared_cache);
    const auto from_b = at_b.compile(root_b / "located.slang", err);
    ASSERT_TRUE(from_b.has_value()) << err.message;
    EXPECT_EQ(from_a->stages[0].spirv, from_b->stages[0].spirv);

    // Nothing should have been written to the fresh dir — that would mean it recompiled.
    size_t written = 0;
    if (std::filesystem::is_directory(empty_writable, ec))
    {
        for (const auto& e : std::filesystem::directory_iterator(empty_writable))
        {
            if (e.path().extension() == ".program") ++written;
        }
    }
    EXPECT_EQ(written, 0u) << "recompiled instead of hitting the relocated cache";

    std::filesystem::remove_all(root_b, ec);
    std::filesystem::remove_all(shared_cache, ec);
    std::filesystem::remove_all(empty_writable, ec);
}

// A truncated or garbage cache file must be ignored and recompiled, never fed to Vulkan.
TEST(ShaderCompiler, CorruptCacheFileFallsBackToRecompiling)
{
    const std::string src = R"(
        [shader("fragment")]
        float4 fragment_main() : SV_Target { return float4(1, 0, 0, 1); }
    )";
    const auto path = write_slang("corrupt.slang", src);
    const auto cache = path.parent_path() / "cache";

    compile_error err;
    shader_compiler compiler(cache, { path.parent_path() });
    const auto good = compiler.compile(path, err);
    ASSERT_TRUE(good.has_value()) << err.message;

    // Truncate every cached program to a header-sized stub.
    for (const auto& entry : std::filesystem::directory_iterator(cache))
    {
        if (entry.path().extension() != ".program") continue;
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out << "SPRG";
    }

    shader_compiler after(cache, { path.parent_path() });
    const auto recovered = after.compile(path, err);
    ASSERT_TRUE(recovered.has_value()) << err.message;
    EXPECT_EQ(good->stages[0].spirv, recovered->stages[0].spirv);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}
