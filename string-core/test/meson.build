test_files = [
    'graph_test.cpp',
    'frame_graph_test.cpp',
    'resource_usage_test.cpp',
    'residency_manager_test.cpp',
    'file_watch_service_test.cpp',
    'shader_compiler_test.cpp',
    'cvar_test.cpp',
    'debug_draw_test.cpp',
    'input_map_test.cpp',
    'normal_matrix_test.cpp',
    'ozz_version_test.cpp',
    'anim_test.cpp'
]

test_dep = [
    dependency('gtest'),
    dependency('gtest_main')
]


test('all_tests',
    executable(
        'run_tests',
        test_files,
        dependencies : [ project_deps, test_dep ],
        # Committed binary fixtures (e.g. the brief-23 two_joint.anim pack) — resolved at
        # compile time so the tests are cwd-independent.
        cpp_args : [ '-DSTRING_TEST_DATA_DIR="' + meson.current_source_dir() / 'data' + '"' ],
        install : false
    )
)