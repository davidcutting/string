project(
    'string-debug',
    'cpp',
    version : '0.0.1',
    license : 'MIT',
    default_options : [
        'cpp_std=c++26', 'buildtype=debugoptimized', 'warning_level=3', 'default_library=static'
    ]
)

# ==================================================
# string-debug — the in-app debug shell
# ==================================================
#
# Console, log view, frame-graph view, profiler HUD, menu bar, panel/docking wiring. OPTIONAL per
# distribution: a shipping client simply doesn't link it, and nothing else changes.
#
# Renderer-agnostic by construction. The frame-graph and profiler views read the engine's
# process-global GraphIntrospect / GpuProfiler handles rather than being handed a renderer, which
# is exactly why those globals exist. Surfaces that DO need a specific renderer's numbers (the
# meshlet cull counters, the per-draw inspector) arrive through slots the app fills — so this
# library stays reusable by a second renderer instead of being welded to the first.
string_dep = dependency('string-core', fallback : ['string-core', 'project_deps'])
ui_dep = dependency('string-ui', fallback : ['string-ui', 'project_deps'])

include_dir = include_directories('include')

debug_lib = static_library(
    'string-debug',
    [ 'src/console.cpp', 'src/panels.cpp', 'src/debug_cvars.cpp' ],
    include_directories : include_dir,
    dependencies : [ string_dep, ui_dep ],
    install : true,
)

install_subdir('include/string', install_dir : 'include')

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ debug_lib ],
    dependencies : [ string_dep, ui_dep ],
)

meson.override_dependency(meson.project_name(), project_deps)
