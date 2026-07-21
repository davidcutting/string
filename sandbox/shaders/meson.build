# Shaders for the application's passes (grid/geometry/ui). Compiled to SPIR-V and installed
# alongside the engine's composite shader under include/string/shaders, so the pass pipelines
# resolve them via STRING_RESOURCES_DIR exactly as before.
shaders = [
    '3d_shader.frag',
    '3d_shader.vert',
    'shadow.vert',
    'sky.vert',
    'sky.frag',
    'cull.comp',
    'grid_2d_shader.vert',
    'grid_2d_shader.frag',
    'ui_shader.frag',
    'ui_shader.vert',
    'text_shader.frag',
    'text_shader.vert',
]

# Shared shader includes (#include "sky.glsl"). -I resolves them; listed as depend_files so a change
# rebuilds the shaders that pull them in.
shader_includes = files('sky.glsl')

shader_targets = []
foreach s : shaders
    shader_targets += custom_target(
        s.split('.')[0] + '_' + s.split('.')[1] + '_shader',
        input : s,
        output : '@PLAINNAME@.spv',
        command : ['glslangValidator', '-I' + meson.current_source_dir(),
                   '--target-env', 'vulkan1.3', '-o', '@OUTPUT@', '@INPUT@'],
        depend_files : shader_includes,
        install : true,
        install_dir : 'include/string/shaders'
    )
endforeach
