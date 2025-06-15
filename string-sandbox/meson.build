project(
    'string-sandbox',
    'cpp',
    version : '0.0.1',
    license: 'MIT',
    default_options : [
        'cpp_std=c++20',
        'buildtype=debug',
        'warning_level=3'
    ]
)

project_description = 'Simple Tiny RenderING engine, the sandbox!'

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
elif get_option('buildtype') == 'debug'
    message('Building in Debug mode')
    release = ['-DSTRING_DEBUG']
elif get_option('buildtype') == 'debugoptimized'
    message('Building in RelWithDebInfo')
    release = ['-DSTRING_DEBUG']
endif

add_project_arguments(release, language : 'cpp')

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
    dependency('string', method: 'pkg-config', required: true, static: true)
]

build_args += [
    '-DPROJECT_NAME=' + meson.project_name(),
    '-DPROJECT_VERSION=' + meson.project_version()
]

exe_target = executable(
    meson.project_name(),
    dependencies : deps,
    include_directories : include_dir,
    sources : src,
    install : false,
    cpp_args : build_args
)

install_subdir('include/string-sandbox', install_dir : 'include')
#install_subdir('assets', install_dir : 'assets')

# --------------------------------------------------

# ======
# Shaders
# ======

#if not meson.is_subproject()
#    subdir('shaders')
#endif
