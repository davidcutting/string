slang_compiler = find_program('slangc')

comp_shader = [
    'test_compute_shader.comp.slang'
]

foreach s : comp_shader
    shader_targets += custom_target(
        s.split('.')[0] + '_' + s.split('.')[1] + '_shader',
        input : s,
        output : '@PLAINNAME@.spv',
        command : ['slangc', '-o', '@OUTPUT@', '@INPUT@'],
        install : true,
        install_dir : 'include/string/shaders/slang'
    )
endforeach