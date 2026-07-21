# Shaders for the application's passes (grid/geometry/ui). Compiled to SPIR-V and installed
# alongside the engine's composite shader under include/string/shaders, so the pass pipelines
# resolve them via STRING_RESOURCES_DIR exactly as before.
shaders = [
    '3d_shader.frag',
    '3d_shader.vert',
    'cull.comp',
    'grid_2d_shader.vert',
    'grid_2d_shader.frag',
    'ui_shader.frag',
    'ui_shader.vert',
    'text_shader.frag',
    'text_shader.vert',
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
