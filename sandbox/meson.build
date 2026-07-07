# ======
# Sandbox application
# ======
#
# The runnable demo: it depends on the engine library (string_dep, exported by
# string-engine/) and supplies the content — the render plan, the concrete scene passes
# (grid/geometry/ui, built entirely from the engine's public API), the demo UI layout, and
# the assets (viking_room, under sandbox/assets). Kept out of the library so the engine can
# be "done" on its own.

sandbox_inc = include_directories('.')

subdir('shaders')

executable(
    'string_demo',
    [
        'main.cpp',
        'demo_scene.cpp',
        'passes/grid_2d_pass.cpp',
        'passes/geometry_pass.cpp',
        'passes/ui_pass.cpp',
    ],
    include_directories : sandbox_inc,
    dependencies : string_dep,
    cpp_args : build_args,
    install : true,
)
