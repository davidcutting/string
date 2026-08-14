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
# Each String library is pulled in as a subproject (see subprojects/, which holds a link per
# sibling project); dependency('string-<x>') resolves to that project's exported `project_deps`
# via its meson.override_dependency(). All of the engine's own wrap dependencies resolve
# transitively from the engine's subprojects/ dir.
string_dep = dependency('string-core', fallback : ['string-core', 'project_deps'])

# The immediate-mode UI kit (layout, widgets, interaction, docking, fonts/text). It depends on
# nothing — the app draws it, so the kit itself never touches the GPU.
ui_dep = dependency('string-ui', fallback : ['string-ui', 'project_deps'])

# The asset pipeline, split by what ships:
#   string-asset       — the cooked-format reader + manifest. Renderer-neutral, importer-free; this
#                        is what a shipped client links.
#   string-asset-tools — the source-asset front-ends (glTF today), the meshletizer/LOD chain, and
#                        the cooked writer. Tool-side; pulls meshoptimizer + MikkTSpace.
# The demo links BOTH because it keeps the in-process cook fallback (cook-on-load when a .cooked is
# missing or stale), which is an editor convenience, not a client one.
asset_dep = dependency('string-asset', fallback : ['string-asset', 'project_deps'])
asset_tools_dep = dependency('string-asset-tools', fallback : ['string-asset-tools', 'project_deps'])

# The asset registry + world layer. The APP owns a registry and injects the tools-backed cook
# provider into it (content_tools.cpp) — the engine libraries never link the importer.
scene_dep = dependency('string-scene', fallback : ['string-scene', 'project_deps'])

# The forward+ renderer: the concrete pass set the demo authors into its RenderPlan. Swapping this
# for a sibling renderer library is the whole point of the split.
render_dep = dependency('string-render-forward', fallback : ['string-render-forward', 'project_deps'])

# The debug shell (optional per distribution) and the game-client UI.
debug_dep = dependency('string-debug', fallback : ['string-debug', 'project_deps'])
client_dep = dependency('string-client', fallback : ['string-client', 'project_deps'])

# JSON, for the scene descriptors. App-side on purpose: descriptors are turned into scenes here,
# and the engine never learns what the format is (brief 18).
# System first (nix: nixpkgs' nlohmann_json — the build sandbox has no network, so a wrap cannot
# download there), else the wrap. allow_fallback:false keeps the first probe from erroring under
# --wrap-mode=nodownload before we reach it.
json_dep = dependency('nlohmann_json', required : false, allow_fallback : false)
if not json_dep.found()
    json_dep = dependency('nlohmann_json')
endif


sandbox_inc = include_directories('.')

executable(
    'string_demo',
    [
        'main.cpp',
        'anim_demo.cpp',
        'content_tools.cpp',
        'debug_cvars.cpp',
        'demo_scene.cpp',
    ],
    include_directories : sandbox_inc,
    dependencies : [ string_dep, ui_dep, render_dep, debug_dep, client_dep, asset_dep, asset_tools_dep,
                    scene_dep, json_dep ],
    install : true,
)
