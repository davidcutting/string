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

# Asset-bake library (brief 04b): the cook logic (meshletize + LOD chain + spatial chunking) +
# cooked-scene format writer/reader + glTF front-end. Shared by the demo's in-process cook fallback,
# the `.#cook` CLI, and the bake tests. Depends on meshopt (meshletize) + the engine (String::Vertex)
# + the sandbox glTF loader (front-end). Built as its own static lib so the demo, CLI, and tests link
# the SAME cook code (the procgen/server contract requires one bake implementation).
assetbake_lib = static_library(
    'assetbake',
    [
        'assetbake/bake.cpp',
        'assetbake/bake_gltf.cpp',
        'assetbake/cooked_io.cpp',
        'assetbake/manifest.cpp',
        'assetbake/scene_loader.cpp',
        'gltf_loader.cpp',
    ],
    include_directories : sandbox_inc,
    link_with : [ mikktspace_lib ],
    dependencies : [ string_dep, meshopt_dep ],
)

executable(
    'string_demo',
    [
        'main.cpp',
        'debug_cvars.cpp',
        'demo_scene.cpp',
        'passes/grid_2d_pass.cpp',
        'passes/geometry_pass.cpp',
        'passes/geometry_pass_probe_gi.cpp',
        'passes/geometry_pass_meshlet.cpp',
        'passes/geometry_pass_lighting.cpp',
        'passes/geometry_pass_transparency.cpp',
        'passes/geometry/sky_component.cpp',
        'passes/geometry/froxel_component.cpp',
        'passes/geometry/ibl_component.cpp',
        'passes/texture_streamer.cpp',
        'passes/geometry_streamer.cpp',
        'passes/ui_pass.cpp',
        'passes/ui_background_pass.cpp',
        'passes/debug_line_pass.cpp',
        'passes/post_pass.cpp',
        'ui/motion.cpp',
        'ui/widgets.cpp',
        'ui/screens.cpp',
        'ui/debug_console.cpp',
        'ui/debug_panels.cpp',
    ],
    include_directories : sandbox_inc,
    link_with : [ mikktspace_lib, assetbake_lib ],
    dependencies : [ string_dep, ktx_dep, meshopt_dep ],
    install : true,
)

# Thin CLI (brief 04b): cooks source glTFs to `.cooked` blobs + maintains the sidecar manifest.
# The cook logic lives entirely in assetbake_lib; this is just an argv front-end. Installed so the
# `.#cook` nix app can invoke it.
executable(
    'string_cook',
    [ 'assetbake/cook_main.cpp' ],
    include_directories : sandbox_inc,
    link_with : [ mikktspace_lib, assetbake_lib ],
    dependencies : [ string_dep, meshopt_dep ],
    install : true,
)

if get_option('tests')
    gtest_dep = dependency('gtest', main : true)
    test('bake_tests',
        executable(
            'bake_tests',
            [ 'assetbake/bake_test.cpp' ],
            include_directories : sandbox_inc,
            link_with : [ mikktspace_lib, assetbake_lib ],
            dependencies : [ string_dep, meshopt_dep, gtest_dep ],
            install : false,
        )
    )
endif
