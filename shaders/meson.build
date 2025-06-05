
glsl_compiler = find_program('glslangValidator')
shaders = [
    '2d_shader.frag',
    '2d_shader.vert',
    '3d_shader.frag',
    '3d_shader.vert',
    'hello_triangle.frag',
    'hello_triangle.vert',
    'ui_shader.frag',
    'ui_shader.vert'
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