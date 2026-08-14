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
# The scene library: the renderer consumes the world/asset layers through their view types
# (and, transitionally, string::Camera which now lives there).
scene_dep = dependency('string-scene', fallback : ['string-scene', 'project_deps'])
# string-asset-tools is GONE from this library's deps (asset-layer split): cook-on-load moved
# behind the asset registry's injected cook_provider, so a shipped client linking this renderer no
# longer links the importer (fastgltf/meshoptimizer/MikkTSpace/ozz-offline) — the brief-18 rule,
# now true in the link graph.

include_dir = include_directories('include')

subdir('shaders')

render_lib = static_library(
    'string-render-forward',
    [
        'src/geometry_pass.cpp',
        'src/geometry_pass_meshlet.cpp',
        'src/geometry_pass_lighting.cpp',
        'src/shadow_maps.cpp',
        'src/sorted_transparency.cpp',
        'src/gtao.cpp',
        'src/probe_gi_component.cpp',
        'src/sky_component.cpp',
        'src/froxel_component.cpp',
        'src/ibl_component.cpp',
        'src/post_pass.cpp',
        'src/ui_pass.cpp',
        'src/ui_background_pass.cpp',
        'src/shadertoy_pass.cpp',
        'src/debug_line_pass.cpp',
        'src/grid_2d_pass.cpp',
        'src/render_cvars.cpp',
        'src/render_debug.cpp',
        'src/scene_bridge.cpp',
    ],
    include_directories : include_dir,
    dependencies : [ string_dep, ui_dep, asset_dep, scene_dep ],
    install : true,
)

install_subdir('include/string', install_dir : 'include')

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ render_lib ],
    dependencies : [ string_dep, ui_dep, asset_dep, scene_dep ],
)

meson.override_dependency(meson.project_name(), project_deps)
