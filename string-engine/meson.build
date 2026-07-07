# The engine library. project(), build_args, and the demo/sandbox live at the repo root
# (../meson.build); this file builds only the library, shaders, tests, and exports string_dep.

# ======
# Dependencies
# ======

# Core deps. On NixOS these are provided (pinned) by the flake; wrapfiles under
# subprojects/ act as the portable fallback for off-Nix targets.
deps = [
    dependency('threads'),
    dependency('spdlog', static : true),
    dependency('glm'),
    dependency('entt'),
    dependency('vulkan'),
]

# --- Vulkan Memory Allocator (header-only) -----------------------------------
# resource_allocator.cpp / allocator.hpp #include <vk_mem_alloc.h>. Try a
# pkg-config/wrap dep first, then fall back to the header on the system include
# path (the Nix `vulkan-memory-allocator` package puts it there).
vma_dep = dependency('VulkanMemoryAllocator', required : false)
if not vma_dep.found()
    assert(compiler.has_header('vk_mem_alloc.h'),
           'vk_mem_alloc.h not found — add VulkanMemoryAllocator (flake pkg or wrap)')
    vma_dep = declare_dependency()
endif
deps += vma_dep

# --- volk (Vulkan meta-loader; header-only, impl compiled in driver.cpp) ------
# No pkg-config is shipped; just ensure the header is reachable.
volk_dep = dependency('volk', required : false)
if not volk_dep.found()
    assert(compiler.has_header('volk.h'),
           'volk.h not found — add vulkan-volk (flake pkg or wrap)')
    volk_dep = declare_dependency()
endif
deps += volk_dep

# --- fastgltf (glTF 2.0 importer; SIMD-accelerated, used for Sponza et al.) ---
# Nix provides it via buildInputs (resolved here by pkg-config/cmake); off-Nix falls back to
# the git wrap under subprojects/, which builds it over simdjson (WrapDB wrap).
deps += dependency('fastgltf', fallback : ['fastgltf', 'fastgltf_dep'])
# fastgltf is a static lib that links simdjson, so the final consumer must link simdjson too
# (static libs don't embed their deps, and neither build path propagates it reliably). List it
# explicitly: Nix resolves it from buildInputs (cmake), off-Nix from the WrapDB wrap.
deps += dependency('simdjson', fallback : ['simdjson', 'simdjson_dep'])

# --- Slang (shader compiler runtime; shader.hpp uses <slang.h>) --------------
slang_dep = dependency('slang', required : false)
if not slang_dep.found()
    slang_lib = compiler.find_library('slang', required : true,
        has_headers : ['slang.h', 'slang-com-ptr.h'])
    slang_dep = declare_dependency(dependencies : slang_lib)
endif
deps += slang_dep

# --- WSI backend (selectable via -Dwsi=) -------------------------------------
wsi = get_option('wsi')
message('WSI backend: ' + wsi)

if wsi == 'glfw'
    deps += dependency('glfw3', static : true)
    platform_src = [ 'src/platform/glfw_window.cpp' ]
    build_args += '-DSTRING_WSI_GLFW'
elif wsi == 'sdl'
    deps += dependency('sdl3')
    platform_src = [ 'src/platform/sdl_window.cpp' ]
    build_args += '-DSTRING_WSI_SDL'
elif wsi == 'wayland'
    # EXPERIMENTAL: the from-scratch client also needs wayland-protocols
    # scanning (wl_scanner) wired up before it will link. Left minimal on
    # purpose; revisit when reviving the raw-Wayland path.
    deps += dependency('wayland-client')
    platform_src = [
        'src/platform/wayland/client.cpp',
        'src/platform/wayland/window.cpp',
        'src/platform/wayland/presenter.cpp',
    ]
    build_args += '-DSTRING_WSI_WAYLAND'
endif

# --------------------------------------------------

# ======
# Library
# ======

include_dir = include_directories('include')

# rewrite-6 engine sources.
# NOTE: the standalone render-graph prototype (its own main()) now lives under
# examples/render_graph.cpp; graph types still live in headers for the future tier.
core_src = [
    'src/application.cpp',
    'src/scene.cpp',
    'src/vulkan/renderer.cpp',
    'src/vulkan/driver.cpp',
    'src/vulkan/device.cpp',
    'src/vulkan/presenter.cpp',
    'src/vulkan/resource_allocator.cpp',
    'src/vulkan/descriptor_allocator.cpp',
    'src/vulkan/command_recorder.cpp',
    'src/vulkan/transfer_batch.cpp',
    'src/vulkan/resource_state.cpp',
    'src/vulkan/vulkan_utils.cpp',
    'src/vulkan/pipeline_builder.cpp',
    # The offscreen->swapchain resolve is the only built-in pass; concrete scene passes
    # (grid/geometry/ui) and their pipelines live in the application (sandbox/).
    'src/vulkan/passes/composite_pass.cpp',
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

install_subdir('include/string', install_dir : 'include')
# NOTE: assets are intentionally NOT copied here. The Nix build symlinks them under
# include/string/assets (see flake.nix postInstall) so large models aren't duplicated into
# every build output. Non-Nix builds point STRING_RESOURCES_DIR at a tree that has them.

# --------------------------------------------------

# ======
# Shaders
# ======

subdir('shaders')

# --------------------------------------------------

# ======
# Package / exported dependency
# ======

pkg_mod = import('pkgconfig')
pkg_mod.generate(
    name : meson.project_name(),
    filebase : meson.project_name(),
    description : project_description,
    libraries : static_target,
    version : meson.project_version()
)

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ static_target ],
    dependencies : deps
)

set_variable(meson.project_name() + '_dep', project_deps)

# --------------------------------------------------

# ======
# Tests
# ======

if get_option('tests') and not meson.is_subproject()
    subdir('test')

    test('all_tests',
        executable(
            'run_tests',
            test_files,
            dependencies : [ project_deps, test_dep ],
            install : false
        )
    )
endif
