test_files = [
    'graph_test.cpp',
    'frame_graph_test.cpp',
    'frame_scratch_test.cpp',
    'resource_usage_test.cpp',
    'residency_manager_test.cpp',
    'file_watch_service_test.cpp',
    'shader_compiler_test.cpp',
    'cvar_test.cpp',
    'debug_draw_test.cpp',
    'input_map_test.cpp'
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
        install : false
    )
)