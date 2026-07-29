test_files = [
    'graph_test.cpp',
    'graph_plan_test.cpp',
    'frame_graph_test.cpp',
    'frame_scratch_test.cpp',
    'layout_test.cpp',
    'layout_dump_test.cpp',
    'ui_interaction_test.cpp',
    'ui_facade_test.cpp',
    'resource_usage_test.cpp',
    'residency_manager_test.cpp',
    'font_test.cpp',
    'file_watch_service_test.cpp',
    'shader_compiler_test.cpp',
    'cvar_test.cpp',
    'debug_draw_test.cpp'
]

test_dep = [
    dependency('gtest'),
    dependency('gtest_main')
]

# font_test loads a DejaVu Sans copy kept beside the test (the engine builds from an isolated
# source subtree, so it can't reach the sandbox asset tree). The sandbox keeps its own copy.
font_test_path = meson.current_source_dir() / 'DejaVuSans.ttf'

test('all_tests',
    executable(
        'run_tests',
        test_files,
        dependencies : [ project_deps, test_dep ],
        cpp_args : [ '-DSTRING_FONT_TEST_PATH="' + font_test_path + '"' ],
        install : false
    )
)