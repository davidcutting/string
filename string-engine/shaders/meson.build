
glsl_compiler = find_program('glslangValidator')
# Only the composite resolve is a built-in engine shader; the scene passes' shaders
# (2d/3d/grid/ui) live with those passes in the application (sandbox/shaders).
shaders = [
    'composite.vert',
    'composite.frag'
]

shader_targets = []
foreach s : shaders
    shader_targets += custom_target(
        s.split('.')[0] + '_' + s.split('.')[1] + '_shader',
        input : s,
        output : '@PLAINNAME@.spv',
        command : ['glslangValidator', '--target-env', 'vulkan1.3', '-o', '@OUTPUT@', '@INPUT@'],
        install : true,
        install_dir : 'include/string/shaders'
    )
endforeach