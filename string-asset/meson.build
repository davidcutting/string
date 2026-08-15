project(
    'string-asset',
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
# string-asset — the SHIPPING half of the asset pipeline
# ==================================================
#
# Loads the engine's preferred offline format (the cooked scene) and nothing else. It deliberately
# does NOT depend on meshoptimizer, fastgltf, or libktx: those belong to string-asset-tools, the
# offline half that CONVERTS source assets into this format. The arrow is one-way
# (string-asset-tools -> string-asset) so a shipped client links a reader, not an importer.
#
# It also does not depend on any renderer. The cooked format is renderer-neutral by design — it
# carries geometry (meshlets, LOD chains, bounds, vertex windows) and leaves material/transform
# binding to whichever renderer loads it.

string_dep = dependency('string-core', fallback : ['string-core', 'project_deps'])

include_dir = include_directories('include')

asset_lib = static_library(
    'string-asset',
    [
        'src/cooked_read.cpp',
        'src/manifest.cpp',
    ],
    include_directories : include_dir,
    dependencies : [ string_dep ],
    install : true,
)

install_subdir('include/string', install_dir : 'include')

# pkg-config, so an OFF-MESON client (CMake, plain make) can resolve this library and its
# String siblings by name; meson consumers keep using the subproject override.
pkg_mod = import('pkgconfig')
pkg_mod.generate(
    name : meson.project_name(),
    filebase : meson.project_name(),
    description : 'String engine cooked-asset format reader + manifest',
    libraries : asset_lib,
    requires : [ 'string-core' ],
    version : meson.project_version(),
)

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ asset_lib ],
    dependencies : [ string_dep ],
)

meson.override_dependency(meson.project_name(), project_deps)
