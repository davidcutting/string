test_files = [
    'graph_test.cpp',
    'layout_test.cpp',
    'resource_usage_test.cpp'
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