#pragma once

#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

#include <string/core/file_watch_service.hpp>
#include <string/core/job_system.hpp>
#include <string/gpu/device.hpp>
#include <string/gpu/pipeline.hpp>
#include <string/gpu/shader_compiler.hpp>
#include <string/gpu/shader_program.hpp>

namespace string::gpu
{

// Owns every hot-reloadable shader_program and drives the compile -> build -> swap lifecycle.
//
// - create() compiles a .slang up front (throws on a *startup* failure — a broken shader at boot
//   is a build error, not a hot-reload event), builds its pipeline, and subscribes the source to
//   the file watcher.
// - On save the watcher fires; the registry recompiles asynchronously on the job pool. A success
//   is staged and applied at the next frame boundary via apply_pending_swaps() (called once per
//   frame before recording): the new pipeline replaces the active one, and the old pipeline is
//   handed to the caller's frame garbage collector so it dies only after the GPU is done with it.
// - A failed compile leaves the active pipeline untouched and records the diagnostic on the
//   program (surfaced by the error overlay).
class shader_program_registry
{
public:
    // Retires a superseded pipeline. The renderer passes a closure that pushes the destroy onto
    // the current frame's garbage_collector ring, so deletion is deferred past in-flight frames.
    using retire_fn = std::function<void(pipeline old)>;

    shader_program_registry(device& device, shader_compiler& compiler,
                            core::file_watch_service& watcher, core::job_system& jobs);

    shader_program_registry(const shader_program_registry&) = delete;
    shader_program_registry& operator=(const shader_program_registry&) = delete;

    // Compile `source_path`, build its pipeline via `builder`, and register it for hot reload.
    // Returns a stable pointer owned by the registry (valid until the registry is destroyed).
    shader_program* create(const std::filesystem::path& source_path, shader_program::build_fn builder);

    // Apply any completed reloads at a frame boundary: swap in rebuilt pipelines and retire the old
    // ones via `retire`. Call once per frame before recording. Cheap when nothing reloaded.
    void apply_pending_swaps(const retire_fn& retire);

    // Snapshot of all current compile errors, for the overlay.
    std::vector<compile_error> current_errors() const;

private:
    // A reload finished compiling; staged until the next frame boundary.
    struct staged_reload
    {
        shader_program* program;
        std::optional<compiled_program> compiled;  // nullopt => compile failed
        compile_error error;
    };

    void on_file_changed(const std::filesystem::path& path);

    device& device_;
    shader_compiler& compiler_;
    core::file_watch_service& watcher_;
    core::job_system& jobs_;

    std::vector<std::unique_ptr<shader_program>> programs_;

    mutable std::mutex staged_mutex_;
    std::vector<staged_reload> staged_;
    // Compiles in flight (kept alive so their futures aren't dropped). Reaped in apply_pending_swaps.
    std::vector<std::future<void>> in_flight_;
};

}  // namespace string::gpu
