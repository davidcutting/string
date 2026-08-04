#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <string/core/file_watch_service.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_compiler.hpp>

namespace string::gpu
{

// A hot-reloadable shader program: a .slang source, the pipeline built from its compiled SPIR-V,
// and the recipe to rebuild that pipeline. Passes create one per pipeline via
// shader_program_registry::create(); it compiles + builds up front and subscribes to the file
// watcher. On save the source recompiles on a job thread; a successful build is queued for swap at
// the next frame boundary (old pipeline retired through the frame garbage collector). A failed
// compile keeps the last-good pipeline and exposes the diagnostic for the error overlay.
class shader_program
{
public:
    // Builds a concrete VkPipeline from freshly compiled SPIR-V + reflection. The builder supplies
    // the compiled program (stages + reflected layout) and the device; the pass fills in the fixed
    // pipeline state it owns (topology, blending, formats, ...) and returns the built pipeline
    // (both VkPipeline and VkPipelineLayout, which the program owns and retires on reload).
    using build_fn = std::function<pipeline(device&, const compiled_program&)>;

    // The current pipeline the pass should bind. Stable pointer for the pass's lifetime; its
    // contents are swapped in place at a frame boundary on a successful reload.
    const pipeline& current() const { return active_; }

    // Diagnostic from the most recent *failed* compile, or nullopt when the last compile succeeded.
    // The error overlay reads this; it clears on the next successful reload.
    const std::optional<compile_error>& error() const { return error_; }

    const std::filesystem::path& source_path() const { return source_path_; }

private:
    friend class shader_program_registry;

    std::filesystem::path source_path_;
    build_fn builder_;
    pipeline active_{};
    std::optional<compile_error> error_;
};

}  // namespace string::gpu
