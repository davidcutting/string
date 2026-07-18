project(
    'string-demo',
    'cpp',
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

sandbox_inc = include_directories('.')

subdir('shaders')

executable(
    'string_demo',
    [
        'main.cpp',
        'demo_scene.cpp',
        'gltf_loader.cpp',
        'passes/grid_2d_pass.cpp',
        'passes/geometry_pass.cpp',
        'passes/ui_pass.cpp',
    ],
    include_directories : sandbox_inc,
    dependencies : string_dep,
    install : true,
)
