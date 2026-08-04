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

    // A cache file should now exist.
    bool cache_has_spv = false;
    for (const auto& entry : std::filesystem::directory_iterator(cache))
    {
        if (entry.path().extension() == ".spv") { cache_has_spv = true; break; }
    }
    EXPECT_TRUE(cache_has_spv);

    // Second compile returns identical SPIR-V (served from cache).
    const auto second = compiler.compile(path, err);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(first->stages.size(), second->stages.size());
    EXPECT_EQ(first->stages[0].spirv, second->stages[0].spirv);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}
