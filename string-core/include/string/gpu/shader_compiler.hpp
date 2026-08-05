#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// STRING_NO_SLANG (meson -Dslang=disabled) builds without Slang at all: no headers, no link, no
// runtime compilation. Programs are then served exclusively from the prebuilt cache. The macro is a
// public compile arg because it removes a member below — every TU must agree on the layout.
#ifndef STRING_NO_SLANG
    #include <slang.h>
    #include <slang-com-ptr.h>
#endif

#include <volk.h>

namespace string::gpu
{

// A single compiled entry point: its SPIR-V and the Vulkan stage it targets.
struct compiled_stage
{
    VkShaderStageFlagBits stage;
    std::string entry_point;      // the Slang entry-point name (e.g. "vertex_main")
    std::vector<uint32_t> spirv;
};

// Reflection-derived layout of a compiled Slang program: the descriptor-set bindings and the
// push-constant range the pipeline layout needs. The engine binds a single global bindless table,
// so in practice reflection reports one set (the bindless table) plus a push-constant block; this
// captures both generically so pipeline_builder never hand-codes a layout again.
struct reflected_layout
{
    // Descriptor-set-layout bindings keyed by set index. Vector index == set number.
    std::vector<std::vector<VkDescriptorSetLayoutBinding>> sets;
    // Push-constant range, if the program declares a push-constant block. Zero size = none.
    VkPushConstantRange push_constant{ 0, 0, 0 };
    bool has_push_constant = false;
};

// The full result of compiling one .slang file: every entry point's SPIR-V plus the shared
// reflected layout across the linked program.
struct compiled_program
{
    std::vector<compiled_stage> stages;
    reflected_layout layout;
};

struct compile_error
{
    std::filesystem::path file;   // the .slang source that failed
    std::string message;          // Slang's diagnostic text (may span several lines)
};

// In-process Slang compiler. Owns the Slang global session (expensive to create — one per app).
// compile() turns a .slang file into SPIR-V + reflection; results are cached to disk keyed by the
// content hash of the source (+ its #include closure) so a cold start with unchanged shaders skips
// recompilation. On failure it returns the diagnostic rather than throwing, so hot-reload can keep
// the last-good pipeline and surface the error.
class shader_compiler
{
public:
    // `cache_dir` is the WRITABLE content-hash-keyed program cache (created if missing).
    // `search_dirs` are the include/import roots handed to Slang, and also what the import-closure
    // hash is computed over. `prebuilt_cache_dir` is an optional READ-ONLY cache shipped with the
    // package: it is consulted when `cache_dir` misses and never written to, which is what lets a
    // -Dslang=disabled build run with no compiler present.
    shader_compiler(std::filesystem::path cache_dir, std::vector<std::filesystem::path> search_dirs,
                    std::filesystem::path prebuilt_cache_dir = {});
    ~shader_compiler();

    shader_compiler(const shader_compiler&) = delete;
    shader_compiler& operator=(const shader_compiler&) = delete;

    // Compile every entry point in `source_path` to SPIR-V and reflect the layout. Thread-safe:
    // each call opens its own Slang session (the global session is the only shared state, and it is
    // documented thread-safe for session creation), so the file-watch job pool can compile off the
    // main thread. Returns nullopt on failure and fills `out_error`.
    //
    // Under STRING_NO_SLANG this only ever reads the caches; a miss fills `out_error` and returns
    // nullopt, which callers already treat as a compile failure.
    std::optional<compiled_program> compile(const std::filesystem::path& source_path,
                                            compile_error& out_error);

    // True if this build can actually compile (i.e. Slang is linked in). Lets callers word errors
    // and disable shader-authoring UI honestly rather than offering a hot-reload that cannot work.
    [[nodiscard]] static constexpr bool can_compile() noexcept
    {
#ifdef STRING_NO_SLANG
        return false;
#else
        return true;
#endif
    }

private:
#ifndef STRING_NO_SLANG
    // Creates the Slang global session on first use; false if creation failed. compile() calls this
    // only after the program cache misses, so a fully-warm cache never constructs it — that is what
    // keeps Slang off the startup path.
    // Locked because compile() runs on the file-watch job pool.
    bool ensure_global_session();

    std::mutex global_session_mutex_;
    Slang::ComPtr<slang::IGlobalSession> global_session_;
#endif
    std::filesystem::path cache_dir_;
    std::filesystem::path prebuilt_cache_dir_;
    std::vector<std::filesystem::path> search_dirs_;
    // Hash of every importable .slang module in the search dirs, folded into every cache key so an
    // edit to an imported module invalidates stale cached SPIR-V. Computed once at construction.
    uint64_t import_closure_hash_ = 0;
};

}  // namespace string::gpu
