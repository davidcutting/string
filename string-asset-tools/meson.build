project(
    'string-asset-tools',
    ['cpp', 'c'],   # c: the vendored MikkTSpace reference lib (third_party/mikktspace/mikktspace.c)
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
# string-asset-tools — the OFFLINE half of the asset pipeline
# ==================================================
#
# Source-asset front-ends (glTF today; OBJ/FBX are additional front-ends onto the same bake_scene
# core), the meshletizer + LOD chain, and the cooked-format writer. Tool-side only: this links
# fastgltf (via the engine) and meshoptimizer, which is exactly why it is a SEPARATE library from
# string-asset. A shipped client depends on string-asset alone and never pulls an importer in.

string_dep = dependency('string-core', fallback : ['string-core', 'project_deps'])
asset_dep = dependency('string-asset', fallback : ['string-asset', 'project_deps'])

# libktx (KTX-Software): the texture cook encodes UASTC then transcodes to BC7 and zstd-deflates
# (see src/texture_cook.cpp). System CMake config first (nix: nixpkgs' ktx-tools), else the `ktx`
# wrap builds it from source — subprojects/ktx.wrap explains why that fallback needs the cmake
# module and its defines rather than the usual [provide] + dependency() form.
#
# The ONLY place libktx is resolved: project_deps re-exports it, so string-render-forward gets it
# transitively. A second cmake.subproject('ktx') in the same build would collide.
ktx_dep = dependency('Ktx', method : 'cmake', modules : ['KTX::ktx'],
                     required : false, allow_fallback : false)
if not ktx_dep.found()
    cmake = import('cmake')
    ktx_opts = cmake.subproject_options()
    ktx_opts.add_cmake_defines({
        'KTX_FEATURE_TESTS': 'OFF',
        'KTX_FEATURE_TOOLS': 'OFF',
        'KTX_FEATURE_DOC': 'OFF',
        'KTX_FEATURE_LOADTEST_APPS': 'OFF',
        # Static, not a DLL. Cross-compiling to Windows, KTX's shared-library target wants a
        # generated exports.def that its CMake does not produce under our cross setup
        # ("cannot find CMakeFiles/ktx.dir/./exports.def"). Static sidesteps that, and one fewer
        # runtime DLL beside the exe is what we want anyway.
        'KTX_FEATURE_STATIC_LIBRARY': 'ON',
        'BUILD_SHARED_LIBS': 'OFF',
    })
    ktx_dep = cmake.subproject('ktx', options : ktx_opts).dependency('ktx')
endif

# meshoptimizer: meshlet build (meshopt_buildMeshlets) + the LOD simplify chain (brief 03).
# Resolved from its CMake config (meshoptimizer::meshoptimizer): on nix from nixpkgs; off-nix from
# the meson wrap.
meshopt_dep = dependency('meshoptimizer',
                         method : 'cmake', modules : ['meshoptimizer::meshoptimizer'],
                         fallback : ['meshoptimizer', 'meshoptimizer_dep'])

# ozz-animation offline builders (SkeletonBuilder/AnimationBuilder; brief 23). ozz is resolved
# ONCE, in string-core/meson.build (single-resolution, the libktx precedent — see the note there
# and in its wrap); string-core exports this override carrying the offline + runtime + base link
# set. The string-core dependency() above already configured that subproject, so the override is
# registered by the time this line runs. The find_library fallback covers the case where
# string-core came from a system install (no subproject, no override) — ozz exports no CMake
# config and no pkg-config, so find_library is the only system probe that works.
ozz_offline_dep = dependency('ozz-animation-offline', required : false)
if not ozz_offline_dep.found()
    cpp = meson.get_compiler('cpp')
    ozz_offline_dep = declare_dependency(dependencies : [
        cpp.find_library('ozz_animation_offline'),
        cpp.find_library('ozz_animation'),
        cpp.find_library('ozz_base'),
    ])
endif

# Vendored MikkTSpace reference lib (tangent generation for meshes without glTF TANGENT). Built as
# its own static lib so the project's -Wall/-Wextra don't drown in this third-party C file.
mikktspace_lib = static_library(
    'mikktspace',
    'third_party/mikktspace/mikktspace.c',
    c_args : ['-w'],
)

# '.' is on the include path for the vendored third_party/ header only; 'include' carries the
# public API.
include_dir = include_directories('include')
private_inc = include_directories('.')

asset_tools_lib = static_library(
    'string-asset-tools',
    [
        'src/bake.cpp',
        'src/bake_gltf.cpp',
        'src/cooked_write.cpp',
        'src/gltf_loader.cpp',
        'src/gltf_skin.cpp',
        'src/scene_cook.cpp',
        'src/texture_cook.cpp',
    ],
    include_directories : [ include_dir, private_inc ],
    link_with : [ mikktspace_lib ],
    dependencies : [ string_dep, asset_dep, meshopt_dep, ktx_dep, ozz_offline_dep ],
    install : true,
)

install_subdir('include/string', install_dir : 'include')

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ asset_tools_lib, mikktspace_lib ],
    dependencies : [ string_dep, asset_dep, meshopt_dep, ktx_dep, ozz_offline_dep ],
)

meson.override_dependency(meson.project_name(), project_deps)

# Thin CLI (brief 04b): cooks source assets to `.cooked` blobs + maintains the sidecar manifest.
# The cook logic lives entirely in the library; this is just an argv front-end. Installed so the
# `.#cook` nix app can invoke it.
executable(
    'string_cook',
    [ 'tools/cook_main.cpp' ],
    dependencies : [ project_deps ],
    install : true,
)

if get_option('tests')
    gtest_dep = dependency('gtest', main : true)
    test('bake_tests',
        executable(
            'bake_tests',
            [ 'test/bake_test.cpp' ],
            dependencies : [ project_deps, gtest_dep ],
            install : false,
        )
    )
    test('winding_tests',
        executable(
            'winding_tests',
            [ 'test/winding_test.cpp' ],
            dependencies : [ project_deps, gtest_dep ],
            install : false,
        )
    )
    # Covers bake_gltf() over a real .glb — the glTF front end has no other test.
    test('glb_embedded_tests',
        executable(
            'glb_embedded_tests',
            [ 'test/glb_embedded_test.cpp' ],
            dependencies : [ project_deps, gtest_dep ],
            install : false,
        )
    )
    test('texture_cook_tests',
        executable(
            'texture_cook_tests',
            [ 'test/texture_cook_test.cpp' ],
            dependencies : [ project_deps, gtest_dep ],
            install : false,
        )
    )
    # Brief 23 M0: proves the ozz offline builders resolve + link (see meson.build's
    # ozz_offline_dep note). The real skeleton/clip cook tests grow beside this.
    test('ozz_offline_tests',
        executable(
            'ozz_offline_tests',
            [ 'test/ozz_offline_test.cpp' ],
            dependencies : [ project_deps, gtest_dep ],
            install : false,
        )
    )
    # Brief 23 M1: the skin cook — quantization, lockstep repack, v4 sections, the .anim pack,
    # and cook-twice byte determinism over a real in-memory skinned .glb.
    test('skin_cook_tests',
        executable(
            'skin_cook_tests',
            [ 'test/skin_cook_test.cpp' ],
            dependencies : [ project_deps, gtest_dep ],
            install : false,
        )
    )
endif
