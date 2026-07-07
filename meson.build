project(
    'string',
    'cpp',
    version : '0.0.1',
    license : 'MIT',
    default_options : [
        'cpp_std=c++23',
        'buildtype=debug',
        'warning_level=3',
        'default_library=static'
    ]
)

project_description = 'Simple Tiny RenderING engine, my hobby engine.'

build_args = [
    '-DPROJECT_NAME=' + meson.project_name(),
    '-DPROJECT_VERSION=' + meson.project_version()
]

compiler = meson.get_compiler('cpp')
message('Compiler: ' + compiler.get_id())

if get_option('buildtype') == 'release'
    message('Building in Release mode')
    build_args += ['-DSTRING_RELEASE', '-O3']
else
    message('Building in Debug mode')
    build_args += ['-DSTRING_DEBUG']
endif

# --------------------------------------------------

# The engine library (mechanism). Builds the static lib + shaders, exports `string_dep`,
# and appends the selected WSI define to build_args.
subdir('string-engine')

# The sandbox application (content): the runnable demo, its render plan, and assets. Depends
# on string_dep. Kept out of the library so the engine can be "done" on its own.
if get_option('demo') and not meson.is_subproject()
    subdir('sandbox')
endif
