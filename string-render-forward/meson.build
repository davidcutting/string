project(
    'string-render-forward',
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
# string-render-forward — the forward+ technique stack
# ==================================================
#
# A RENDERER: the concrete set of passes that turns a scene into pixels. Meshlet geometry with
# two-phase occlusion, clustered (froxel) lighting, CSM, dynamic-sky IBL, probe GI, GTAO, post, and
# the UI/debug-line/composite passes that go with them — plus the shaders they compile.
#
# It is a separate library from string-core because the engine is swap-INVARIANT: the GPU
# abstraction and the frame-graph executor are the substrate every renderer authors into, and
# `RenderPlan` is already the seam that swaps one renderer for another (the `ui` scene proves it —
# a completely different pass set on the same engine). Adding a deferred or a 2D renderer means
# adding a sibling of this library, not editing it.
#
# The CVars that tune these passes live here too (render_cvars.hpp) — they ARE this renderer's
# tuning surface, and a different renderer would expose a different set.

string_dep = dependency('string-core', fallback : ['string-core', 'project_deps'])
ui_dep = dependency('string-ui', fallback : ['string-ui', 'project_deps'])
asset_dep = dependency('string-asset', fallback : ['string-asset', 'project_deps'])
# The in-process cook fallback (cook-on-load when a .cooked is missing or stale) is an editor
# convenience the demo relies on. A shipped client would load cooked-only and drop this.
asset_tools_dep = dependency('string-asset-tools', fallback : ['string-asset-tools', 'project_deps'])

# libktx: cooked .ktx2 textures are already BC7, so the streamer uploads blocks with no transcode.
# System KTX-Software CMake config (nix: nixpkgs' ktx-tools). Off-nix there is no system install and
# this probe fails — libktx then arrives transitively through asset_tools_dep, which owns the wrap
# and re-exports it. Resolving it a second time here would mean two cmake.subproject('ktx') calls in
# one build.
ktx_dep = dependency('Ktx', method : 'cmake', modules : ['KTX::ktx'],
                     required : false, allow_fallback : false)
if not ktx_dep.found()
    ktx_dep = declare_dependency()
endif

include_dir = include_directories('include')

subdir('shaders')

render_lib = static_library(
    'string-render-forward',
    [
        'src/geometry_pass.cpp',
        'src/geometry_pass_probe_gi.cpp',
        'src/geometry_pass_meshlet.cpp',
        'src/geometry_pass_lighting.cpp',
        'src/geometry_pass_transparency.cpp',
        'src/sky_component.cpp',
        'src/froxel_component.cpp',
        'src/ibl_component.cpp',
        'src/texture_streamer.cpp',
        'src/geometry_streamer.cpp',
        'src/scene_loader.cpp',
        'src/post_pass.cpp',
        'src/ui_pass.cpp',
        'src/ui_background_pass.cpp',
        'src/shadertoy_pass.cpp',
        'src/debug_line_pass.cpp',
        'src/grid_2d_pass.cpp',
        'src/render_cvars.cpp',
        'src/render_debug.cpp',
    ],
    include_directories : include_dir,
    dependencies : [ string_dep, ui_dep, asset_dep, asset_tools_dep, ktx_dep ],
    install : true,
)

install_subdir('include/string', install_dir : 'include')

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ render_lib ],
    dependencies : [ string_dep, ui_dep, asset_dep, asset_tools_dep, ktx_dep ],
)

meson.override_dependency(meson.project_name(), project_deps)
