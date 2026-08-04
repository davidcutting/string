project(
    'string-client',
    'cpp',
    version : '0.0.1',
    license : 'MIT',
    default_options : [
        'cpp_std=c++26', 'buildtype=debugoptimized', 'warning_level=3', 'default_library=static'
    ]
)

# ==================================================
# string-client — the game client's UI
# ==================================================
#
# Nameplates, inventory, action bar, chat, the ping radial — plus the game's palette and the
# gameplay widgets built on the UI kit. This is where "what the player sees" lives, separate from
# the debug shell so a shipping build can take one without the other.
#
# The line it draws with string-ui is the one the theme header already described: the KIT owns UI
# vocabulary (widgets, interaction, motion, the Theme type), the CLIENT owns game vocabulary — item
# rarity, health and cast fills, hostile vs friendly. A UI kit has no business knowing what "epic"
# means.
string_dep = dependency('string-core', fallback : ['string-core', 'project_deps'])
ui_dep = dependency('string-ui', fallback : ['string-ui', 'project_deps'])
include_dir = include_directories('include')

client_lib = static_library(
    'string-client',
    [ 'src/screens.cpp', 'src/widgets.cpp', 'src/theme.cpp' ],
    include_directories : include_dir,
    dependencies : [ string_dep, ui_dep ],
    install : true,
)

install_subdir('include/string', install_dir : 'include')

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ client_lib ],
    dependencies : [ string_dep, ui_dep ],
)

meson.override_dependency(meson.project_name(), project_deps)
