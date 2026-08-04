project(
    'slang',
    'cpp',
    version : '2026.12.0.1',
    license : 'Apache-2.0',
)

# Prebuilt Slang: shared libs under lib/, headers under include/. Slang ships no static
# library, so we link libslang dynamically. It stays an in-process compiler (used for shader
# hot-reload); at runtime it dlopen()s its sibling libs (libslang-llvm, libslang-glslang, …)
# from the same directory, so the whole lib/ must be on the runtime search path.
cpp = meson.get_compiler('cpp')

slang_inc = include_directories('include')
slang_libdir = meson.current_source_dir() / 'lib'

slang_lib = cpp.find_library(
    'slang',
    dirs : slang_libdir,
    has_headers : ['slang.h', 'slang-com-ptr.h'],
    header_include_directories : slang_inc,
)

slang_dep = declare_dependency(
    include_directories : slang_inc,
    dependencies : slang_lib,
)

meson.override_dependency('slang', slang_dep)
