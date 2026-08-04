project(
    'string-ui',
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
# string-ui — the immediate-mode UI kit
# ==================================================
#
# Layout, widgets, interaction, panels/docking, theming, and the font/text stack underneath them.
#
# It depends on NOTHING — not the engine, not Vulkan, not a windowing backend. That is not an
# accident to be preserved by vigilance; it falls out of the design. The kit produces a laid-out
# tree and consumes plain input structs, so whoever draws it (today the renderer's ui_pass) owns
# every GPU concern. That is also what makes the whole kit testable headlessly, which is why the
# layout/widget/interaction gates can run in CI with no device at all.
#
# `layout`, `font`, `dynamic_font`, `text_shaper`, `text_measurer` and `layout_dump` moved here from the engine's
# core/ with the rest of the kit: they are the UI's own foundation, not general engine services,
# and nothing outside the UI ever used them.

include_dir = include_directories('include')

ui_lib = static_library(
    'string-ui',
    [
        'src/font.cpp',
        'src/dynamic_font.cpp',
        'src/motion.cpp',
        'src/interaction.cpp',
        'src/ui.cpp',
        'src/panel.cpp',
        'src/workspace.cpp',
        'src/text_edit.cpp',
        'src/text_shaper.cpp',
        'src/widgets.cpp',
        'src/theme.cpp',
    ],
    include_directories : include_dir,
    install : true,
)

install_subdir('include/string', install_dir : 'include')

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ ui_lib ],
)

meson.override_dependency(meson.project_name(), project_deps)

if get_option('tests')
    subdir('test')
endif
