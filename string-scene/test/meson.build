gtest_dep = dependency('gtest', main : true)

scene_tests = executable(
    'string_scene_tests',
    [
        'asset_registry_test.cpp',
        'camera_test.cpp',
        'world_test.cpp',
    ],
    dependencies : [ project_deps, gtest_dep ],
)

test('string-scene', scene_tests)
