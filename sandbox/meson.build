project(
    'string-demo',
    ['cpp', 'c'],   # c: the vendored MikkTSpace reference lib (third_party/mikktspace/mikktspace.c)
    version : '0.0.1',
    license : 'MIT',
    default_options : [
        'cpp_std=c++26',
        'buildtype=debugoptimized',
        'warning_level=3',
    ]
)

# ======
# Sandbox application
# ======
#
# The runnable demo — deliberately its OWN meson project that consumes the engine as an
# external dependency, so it doubles as the worked example of "how to include String" and as
# the integration test that the engine's public API + exported dependency actually work.
#
# string-engine is pulled in as a subproject (see subprojects/string-engine, a link to the
# sibling engine project); dependency('string-engine') resolves to the engine's exported
# `project_deps` via its meson.override_dependency(). All of the engine's own wrap
# dependencies resolve transitively from the engine's subprojects/ dir.
string_dep = dependency('string-engine', fallback : ['string-engine', 'project_deps'])

# libktx (KTX-Software): the sandbox loader reads cooked .ktx2 textures and transcodes them to
# BC7 (see passes/geometry_pass.cpp). The engine core stays KTX-agnostic — only the app links it.
# Resolved from its CMake config (KTX::ktx): on nix from nixpkgs' ktx-tools; off-nix from a
# system KTX-Software install (there's no meson wrap — its CMake build isn't wired as a subproject).
ktx_dep = dependency('Ktx', method : 'cmake', modules : ['KTX::ktx'])

# meshoptimizer: meshlet build (meshopt_buildMeshlets) + the LOD simplify chain (brief 03). Like
# ktx, the engine stays generic — meshletization is an app-side asset step. Resolved from its CMake
# config (meshoptimizer::meshoptimizer): on nix from nixpkgs; off-nix from the meson wrap.
meshopt_dep = dependency('meshoptimizer',
                         method : 'cmake', modules : ['meshoptimizer::meshoptimizer'],
                         fallback : ['meshoptimizer', 'meshoptimizer_dep'])

sandbox_inc = include_directories('.')

# Vendored MikkTSpace reference lib (tangent generation for meshes without glTF TANGENT). Built as
# its own static lib so the project's -Wall/-Wextra don't drown in this third-party C file.
mikktspace_lib = static_library(
    'mikktspace',
    'third_party/mikktspace/mikktspace.c',
    c_args : ['-w'],
)

subdir('shaders')

executable(
    'string_demo',
    [
        'main.cpp',
        'demo_scene.cpp',
        'gltf_loader.cpp',
        'passes/grid_2d_pass.cpp',
        'passes/geometry_pass.cpp',
        'passes/meshlet_builder.cpp',
        'passes/texture_streamer.cpp',
        'passes/geometry_streamer.cpp',
        'passes/ui_pass.cpp',
    ],
    include_directories : sandbox_inc,
    link_with : [ mikktspace_lib ],
    dependencies : [ string_dep, ktx_dep, meshopt_dep ],
    install : true,
)
