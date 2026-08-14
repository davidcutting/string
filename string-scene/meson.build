project(
    'string-scene',
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

# ==================================================
# string-scene — the runtime asset registry + the world
# ==================================================
#
# Two namespaces, one library:
#   string::assets — the runtime asset layer: stable asset ids, each asset a named set of
#                    resources (logical handles to their GPU backing). Owns the content heaps,
#                    streamers and residency. Parses cooked files via string-asset; never links
#                    an importer (string-asset-tools stays tools-only).
#   string::scene  — the world: entities in flat handle-based SoA tables (spec: NOT ECS),
#                    transforms, lights, camera, environment, animation playback. Owns zero GPU
#                    objects, so a headless server can tick a world.
#
# Render passes never see these types — they consume two value-snapshot views
# (assets::gpu_view, scene::frame_view) published per frame. That one-way arrow
# (render-forward -> string-scene, views only) is the boundary this library exists to hold.

string_dep = dependency('string-core', fallback : ['string-core', 'project_deps'])
asset_dep = dependency('string-asset', fallback : ['string-asset', 'project_deps'])

# libktx: the texture pool streams cooked .ktx2 (already BC7 — upload with no transcode). System
# KTX-Software CMake config (nix: nixpkgs' ktx-tools). Off-nix there is no system install and this
# probe fails — the off-nix build is already blocked on libktx (the Windows-blocker note); when
# that is fixed, this library owns the wrap.
ktx_dep = dependency('Ktx', method : 'cmake', modules : ['KTX::ktx'],
                     required : false, allow_fallback : false)
if not ktx_dep.found()
    ktx_dep = declare_dependency()
endif

include_dir = include_directories('include')

scene_lib = static_library(
    'string-scene',
    [
        'src/asset_registry.cpp',
        'src/camera.cpp',
        'src/mesh_heap.cpp',
        'src/world.cpp',
        'src/texture_pool.cpp',
    ],
    include_directories : include_dir,
    dependencies : [ string_dep, asset_dep, ktx_dep ],
    install : true,
)

install_subdir('include/string', install_dir : 'include')

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ scene_lib ],
    dependencies : [ string_dep, asset_dep, ktx_dep ],
)

meson.override_dependency(meson.project_name(), project_deps)

if get_option('tests')
    subdir('test')
endif
