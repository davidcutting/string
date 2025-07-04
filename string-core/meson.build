project(
    'string',
    'cpp',
    version : '0.0.1',
    license: 'MIT',
    default_options : [
        'cpp_std=c++20',
        'buildtype=debug',
        'warning_level=3'
    ]
)

project_description = 'Simple Tiny RenderING engine, my hobby engine.'

build_args = [
]

compiler = meson.get_compiler('cpp')
if compiler.get_id() == 'gcc'
    message('Compiler: GCC')
elif compiler.get_id() == 'clang'
    message('Compiler: LLVM/Clang')
endif

if get_option('buildtype') == 'release'
    message('Building in Release mode')
    release = ['-DSTRING_RELEASE']
    build_args += ['-o3']
elif get_option('buildtype') == 'debugoptimized'
    message('Building in RelWithDebInfo')
    release = ['-DSTRING_DEBUG']
    build_args += ['-o3']
elif get_option('buildtype') == 'debug'
    message('Building in Debug mode')
    release = ['-DSTRING_DEBUG']
endif

add_project_arguments(release, language : 'cpp')

# --------------------------------------------------

# ======
# Target
# ======

include_dir = include_directories('include')

src = [
    #'src/platform/window.cpp',
    #'src/platform/platform.cpp',
]

deps = [
    dependency('threads'),
    dependency('spdlog'),
    dependency('glfw3'),
    dependency('vulkan')
]

build_args += [
    '-DPROJECT_NAME=' + meson.project_name(),
    '-DPROJECT_VERSION=' + meson.project_version()
]

static_target = static_library(
    meson.project_name(),
    dependencies: deps,
    include_directories : include_dir,
    sources : src,
    install : true,
    cpp_args : build_args
)

install_subdir('include/string', install_dir : 'include')

# --------------------------------------------------

# ======
# Project
# ======

pkg_mod = import('pkgconfig')
pkg_mod.generate(
    name : meson.project_name(),
    filebase : meson.project_name(),
    description : project_description,
    libraries : static_target,
    version : meson.project_version()
)

project_deps = declare_dependency(
    include_directories : include_dir,
    link_with : [ static_target ]
)

set_variable(meson.project_name() + '_dep', project_deps)

# --------------------------------------------------

# ======
# Test Exe
# ======

#render_test_src = [
#    'test/render_test.cpp',
#]
#
#render_test = executable(
#    meson.project_name() + '_render_test',
#    dependencies : project_deps,
#    sources : render_test_src,
#    install : true,
#    cpp_args : build_args
#)

net_test_src = [
    'test/net_test.cpp',
]

net_test = executable(
    meson.project_name() + '_net_test',
    dependencies : project_deps,
    sources : net_test_src,
    install : true,
    cpp_args : build_args
)