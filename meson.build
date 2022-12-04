project(
    'renderer',
    'cpp',
    version : '0.0.1',
    default_options : [
        'cpp_std=c++20',
        'warning_level=3'
    ]
)

project_description = 'A Vulkan renderer.'

build_args = [
]

# --------------------------------------------------

# ======
# Target
# ======

include_dir = include_directories('include')

src = [
    'src/main.cpp'
]

deps = [
    dependency('threads'),
    dependency('spdlog'),
    dependency('glm'),
    dependency('glfw3'),
    dependency('vulkan')
]

build_args += [
    '-DPROJECT_NAME=' + meson.project_name(),
    '-DPROJECT_VERSION=' + meson.project_version()
]

renderer = executable(
    meson.project_name(),
    dependencies: deps,
    include_directories : include_dir,
    sources : src,
    install : true,
    c_args : build_args
)

pkg_mod = import('pkgconfig')
pkg_mod.generate(
    name : meson.project_name(),
    filebase : meson.project_name(),
    description : project_description,
    subdirs : meson.project_name(),
    #libraries : [shared_target, static_target]
)

# --------------------------------------------------

# ======
# Shaders
# ======

#glsllang = find_program('glslangvalidator')
#shaders = 'shaders'
#shader_targets = []
#foreach s : shaders
#  shaders += custom_target(
#    'shader @0@'.format(s),
#    command : [glsllang, '-V', '@INPUT@', '-o',  '@OUTPUT@'],
#    input : s, 
#    output : '@BASENAME@.spv'.
#  )
#endforeach

# --------------------------------------------------

# ======
# Project
# ======

project_deps = declare_dependency(
    include_directories : include_dir,
    sources : src,
    link_with : renderer
)

set_variable(meson.project_name() + '_dep', project_deps)

# --------------------------------------------------

# ======
# Tests
# ======

#if not meson.is_subproject()
#  add_languages('cpp')
#  subdir('test')
#
#  test('all_tests',
#    executable(
#      'run_tests',
#      test_files,
#      dependencies : [project_deps, test_dep],
#      install : false
#    )
#  )
#endif