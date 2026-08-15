project(
    'string-core',
    'cpp',
    version : '0.0.1',
    license : 'MIT',
    default_options : [
        'cpp_std=c++26',
        'buildtype=debugoptimized',
        'warning_level=3',
        'default_library=static'
    ]
)

project_description = 'Simple Tiny RenderING engine.'

# ==================================================
# Defines
# ==================================================

# public_args: defines that also belong on the installed pkg-config (extra_cflags), so
# external consumers (e.g. sandbox/) compile the public headers against the same platform,
# config, and WSI choices. PROJECT_NAME/VERSION stay build-only.
public_args = []
build_args = [
    '-DPROJECT_NAME=' + meson.project_name(),
    '-DPROJECT_VERSION=' + meson.project_version()
]

compiler = meson.get_compiler('cpp')
system = host_machine.system()
message('Compiler: ' + compiler.get_id())

# Optimization is owned by buildtype; only the config define is ours to set.
if get_option('buildtype') == 'release'
    message('Building in Release mode')
    public_args += '-DSTRING_RELEASE'
else
    message('Building in Debug mode')
    public_args += '-DSTRING_DEBUG'
endif

if system == 'windows'
    message('Building for Windows')
    public_args += '-DSTRING_PLATFORM_WINDOWS'
    # windows.h defines min/max as object-like macros, so any `std::min(a, b)` downstream of it
    # expands to `std::(a, b)` — "expected unqualified-id". It bit application.cpp's scene-switch
    # parser. Public, not build-only: consumers include our headers and hit the same collision.
    public_args += '-DNOMINMAX'
elif system == 'linux'
    message('Building for Linux')
    public_args += '-DSTRING_PLATFORM_LINUX'
endif

build_args += public_args

# ==================================================
# Dependencies
# ==================================================

deps = [
    dependency('threads'),
    dependency('spdlog', static : true),
    dependency('glm'),
    dependency('entt'),
    # Vulkan headers only — no loader link. volk loads the loader dynamically at runtime,
    # so we never link libvulkan at build time (leaner, and hermetic across platforms).
    dependency('VulkanHeaders'),
]

# --- VulkanMemoryAllocator (header-only; impl compiled in the engine) ---------
# nixpkgs ships VMA header-only with no pkg-config/cmake meson can resolve by name, so on
# nix (where wraps can't download) we fall back to the system header. Off-nix, where the
# header isn't on the system path, we use the wrap. allow_fallback:false keeps the first
# probe from erroring on --wrap-mode=nodownload before we get to the header check.
vma_dep = dependency('VulkanMemoryAllocator', required : false, allow_fallback : false)
if not vma_dep.found()
    if compiler.has_header('vk_mem_alloc.h')
        vma_dep = declare_dependency()                        # nix: system header
    else
        vma_dep = dependency('VulkanMemoryAllocator', static : true)  # off-nix: wrap
    endif
endif
deps += vma_dep

# --- volk (Vulkan meta-loader) -----------------------------------------------
# Provided by the volk wrap (header-only; the engine compiles the impl in driver.cpp).
deps += dependency('volk')

# --- Slang (shader compiler runtime; shader.hpp uses <slang.h>) --------------
# Prebuilt shared lib; runs in-process for shader hot-reload. System install first (nix:
# nixpkgs' shader-slang), else a per-platform wrap. Slang ships prebuilts, not source, and a
# meson wrap-file pins exactly one archive — so the platforms are separate wraps chosen HERE by
# an explicit subproject() call. They deliberately carry no [provide]: two wraps providing
# `slang` would be a duplicate-provider conflict.
#
# -Dslang=disabled is SHIP MODE: Slang is neither linked nor compiled against, and shader_compiler
# serves programs only from the prebuilt cache. STRING_NO_SLANG is a PUBLIC arg because it changes
# the shader_compiler class layout (the global-session member disappears) — every TU that includes
# the header must agree, or it is an ODR violation.
if get_option('slang').disabled()
    # Both lists, like STRING_PROFILE below: public_args reaches consumers via project_deps,
    # build_args reaches this library's own TUs. `build_args += public_args` already ran above.
    public_args += '-DSTRING_NO_SLANG'
    build_args += '-DSTRING_NO_SLANG'
    message('Slang DISABLED: cache-only shader loading. The package must carry a prebuilt cache.')
else
    slang_dep = dependency('slang', required : false, allow_fallback : false)
    if not slang_dep.found()
        slang_wraps = {
            'linux'   : 'slang-linux-x86_64',
            'windows' : 'slang-windows-x86_64',
        }
        if not slang_wraps.has_key(system)
            error('No prebuilt Slang wrap for host system "' + system + '". Add one from the Slang ' +
                  'releases page (same shape as slang-linux-x86_64.wrap), or install Slang so ' +
                  'dependency(\'slang\') finds it.')
        endif
        slang_dep = subproject(slang_wraps[system]).get_variable('slang_dep')
    endif
    deps += slang_dep
endif

# --- ozz-animation (skeletal animation; brief 23) -----------------------------
# Runtime half (ozz_base + ozz_animation: sampling, blending, local-to-model) links into the
# engine for the string::anim module. ozz exports NO CMake config package and no pkg-config —
# its install is a plain lib/ + include/ — so the system probe is find_library + has_header
# (nix: the hash-pinned derivation in flake.nix), and the fallback drives ozz's CMake through
# the wrap (upstream's own recommended integration).
#
# The ONLY place ozz is resolved (the libktx single-resolution precedent: a second
# cmake.subproject('ozz-animation') in the same super-build would collide). The OFFLINE half
# (ozz_animation_offline — SkeletonBuilder/AnimationBuilder, cook-time only) is resolved here
# for that reason but NOT linked into the engine; string-asset-tools consumes it via the
# 'ozz-animation-offline' override below.
ozz_base_lib = compiler.find_library('ozz_base', required : false)
ozz_anim_lib = compiler.find_library('ozz_animation', required : false)
ozz_offline_lib = compiler.find_library('ozz_animation_offline', required : false)
if ozz_base_lib.found() and ozz_anim_lib.found() and ozz_offline_lib.found() and \
   compiler.has_header('ozz/animation/runtime/skeleton.h')
    # Static archives: dependents before their dependencies on the link line.
    ozz_runtime_dep = declare_dependency(dependencies : [ ozz_anim_lib, ozz_base_lib ])
    ozz_offline_dep = declare_dependency(
        dependencies : [ ozz_offline_lib, ozz_anim_lib, ozz_base_lib ])
else
    cmake = import('cmake')
    ozz_opts = cmake.subproject_options()
    ozz_opts.add_cmake_defines({
        'ozz_build_samples': 'OFF',
        'ozz_build_howtos': 'OFF',
        'ozz_build_tests': 'OFF',
        'ozz_build_tools': 'OFF',   # also force-disables the fbx/gltf importer targets
        'ozz_build_data': 'OFF',
        # Default ON appends per-config suffixes (libozz_animation_r.a); every consumer
        # (and the nix derivation) expects the plain names.
        'ozz_build_postfix': 'OFF',
        # 0.17.0 hard-enables warnings-as-errors; a new compiler warning must not break
        # the dependency build.
        'CMAKE_COMPILE_WARNING_AS_ERROR': 'OFF',
        'BUILD_SHARED_LIBS': 'OFF',
    })
    ozz_sub = cmake.subproject('ozz-animation', options : ozz_opts)
    ozz_runtime_dep = declare_dependency(dependencies : [
        ozz_sub.dependency('ozz_animation'),
        ozz_sub.dependency('ozz_base'),
    ])
    ozz_offline_dep = declare_dependency(dependencies : [
        ozz_sub.dependency('ozz_animation_offline'),
        ozz_sub.dependency('ozz_animation'),
        ozz_sub.dependency('ozz_base'),
    ])
endif
deps += ozz_runtime_dep
meson.override_dependency('ozz-animation-offline', ozz_offline_dep)

# --- WSI backend (selectable via -Dwsi=) -------------------------------------
wsi = get_option('wsi')
message('WSI backend: ' + wsi)

if wsi == 'sdl'
    deps += dependency('sdl3')
    platform_src = [ 'src/platform/sdl_window.cpp' ]
    build_args += '-DSTRING_WSI_SDL'
elif wsi == 'wayland'
    # EXPERIMENTAL, Linux-only: the from-scratch client still needs wayland-protocols
    # scanning (wayland-scanner, shipped by the wayland wrap) wired up before it will
    # link. Left minimal on purpose; revisit when reviving the raw-Wayland path.
    if system != 'linux'
        error('wsi=wayland is Linux-only')
    endif
    deps += dependency('wayland-client')
    platform_src = [
        'src/platform/wayland/client.cpp',
        'src/platform/wayland/window.cpp',
        'src/platform/wayland/presenter.cpp',
    ]
    build_args += '-DSTRING_WSI_WAYLAND'
elif wsi == 'glfw'
    # The glfw backend source (glfw_window.cpp) is kept in-tree but is not wired into the
    # wrap-based build (no glfw wrap). Reintroduce a glfw wrap here to revive it.
    error('wsi=glfw is not supported in the wrap-based build; use wsi=sdl')
endif

# --- Tracy profiler client (optional; -Dtracy=true) --------------------------
# STRING_PROFILE gates the profiler.hpp macros and is public (it appears in a public
# header), so it rides along on public_args. tracy_enable turns on the client in the wrap.
if get_option('tracy')
    message('Tracy profiler: enabled')
    # On nix the client comes from the nixpkgs `tracy` package, which exports a CMake config
    # (Tracy::TracyClient) but no pkg-config and no meson-name match — probe that first. Off-nix
    # (no system Tracy) fall back to the meson wrap, which builds the vendored client with the
    # client compiled in (tracy_enable=true). allow_fallback:false keeps the first probe from
    # erroring under --wrap-mode=nodownload before we reach the wrap.
    tracy_dep = dependency('Tracy', method : 'cmake', modules : ['Tracy::TracyClient'],
                           required : false, allow_fallback : false)
    if not tracy_dep.found()
        tracy_dep = dependency('tracy', default_options : ['tracy_enable=true'])
    endif
    deps += tracy_dep
    public_args += '-DSTRING_PROFILE'
    build_args += '-DSTRING_PROFILE'
endif

# --------------------------------------------------

# ==================================================
# Library
# ==================================================

include_dir = include_directories('include')

core_src = [
    'src/application.cpp',
    'src/anim/anim_pack.cpp',
    'src/anim/anim_set.cpp',
    'src/anim/player.cpp',
    'src/scene.cpp',
    'src/core/job_system.cpp',
    'src/core/cvar.cpp',
    'src/core/tonemap.cpp',
    'src/core/file_watch_service.cpp',
    'src/platform/input_map.cpp',
    'src/platform/input_names.cpp',
    'src/vulkan/frame_graph.cpp',
    'src/vulkan/renderer.cpp',
    'src/vulkan/passes/composite_pass.cpp',
    'src/gpu/vk_check.cpp',
    'src/gpu/driver.cpp',
    'src/gpu/device.cpp',
    'src/gpu/presenter.cpp',
    'src/gpu/resource_allocator.cpp',
    'src/gpu/descriptor_allocator.cpp',
    'src/gpu/residency_manager.cpp',
    'src/gpu/command_recorder.cpp',
    'src/gpu/submission.cpp',
    'src/vulkan/transfer_batch.cpp',
    'src/vulkan/gpu_profiler.cpp',
    'src/vulkan/graph_introspect.cpp',
    'src/debug_draw.cpp',
    'src/vulkan/resource_state.cpp',
    'src/vulkan/scene_registry.cpp',
    'src/vulkan/content_root.cpp',
    'src/platform/user_dirs.cpp',
    'src/gpu/pipeline_builder.cpp',
    'src/gpu/shader_compiler.cpp',
    'src/gpu/shader_program_registry.cpp',
    # The offscreen->swapchain resolve is the only built-in pass; concrete scene passes
    # (grid/geometry/ui) and their pipelines live in the application (sandbox/).
]

src = core_src + platform_src

static_target = static_library(
    meson.project_name(),
    dependencies : deps,
    include_directories : include_dir,
    sources : src,
    install : true,
    cpp_args : build_args
)

# The public API is the installed tree MINUS the headers only this library's .cpps include:
# vk_check (error-reporting internals) and the wayland WSI backend. They lived under include/
# for the src tree's include-path convenience and were ACCIDENTAL API — nothing outside
# string-core references either (verified before excluding). resource_state.hpp stays installed
# because frame_graph.hpp (public) embeds a resource_state_tracker; it is structurally public.
install_subdir('include/string', install_dir : 'include',
               exclude_files : [ 'gpu/vk_check.hpp' ],
               exclude_directories : [ 'platform/wayland' ])
# NOTE: assets are intentionally NOT copied here

# --------------------------------------------------

# ==================================================
# Shaders
# ==================================================

subdir('shaders')

# --------------------------------------------------

# ==================================================
# Package / exported dependency
# ==================================================

pkg_mod = import('pkgconfig')
pkg_mod.generate(
    name : meson.project_name(),
    filebase : meson.project_name(),
    description : project_description,
    libraries : static_target,
    version : meson.project_version(),
    extra_cflags : public_args,
)

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ static_target ],
    dependencies : deps,
    # Propagate the public defines (config/platform/WSI, and STRING_PROFILE when tracy is on)
    # so external consumers compile their engine-header usage against the same choices.
    compile_args : public_args,
)

meson.override_dependency(meson.project_name(), project_deps)

# --------------------------------------------------

# ==================================================
# Tests, Benchmarks, Examples
# ==================================================

if get_option('tests')
  subdir('test')
endif
#if get_option('benchmarks')
#  subdir('bench')
#endif
#if get_option('examples')
#  subdir('examples')
#endif
