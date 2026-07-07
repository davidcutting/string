# Meson build overlay for spnda/fastgltf (v0.8.0), which upstream only ships with CMake.
# fastgltf is small: three translation units over simdjson, with a header-only public API.
# simdjson is a PRIVATE dependency — core.hpp only forward-declares simdjson::dom types, so
# consumers never need simdjson on their include path.

project('fastgltf', 'cpp', version : '0.8.0', license : 'MIT',
        default_options : ['cpp_std=c++17'])

# simdjson: system dep if present, else the WrapDB wrap under subprojects/.
simdjson_dep = dependency('simdjson', fallback : ['simdjson', 'simdjson_dep'])

fastgltf_inc = include_directories('include')

fastgltf_lib = static_library(
    'fastgltf',
    sources : ['src/fastgltf.cpp', 'src/base64.cpp', 'src/io.cpp'],
    include_directories : fastgltf_inc,
    dependencies : simdjson_dep,   # PRIVATE: not re-exported below
    install : false,
)

# Public dependency: headers + the compiled library, WITHOUT simdjson (it stays private).
fastgltf_dep = declare_dependency(
    include_directories : fastgltf_inc,
    link_with : fastgltf_lib,
)

meson.override_dependency('fastgltf', fastgltf_dep)
