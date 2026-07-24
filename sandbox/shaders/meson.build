# Shaders for the application's passes, compiled in-process from Slang (brief 01): the engine
# compiles them at startup and recompiles on save for hot-reload (via string::gpu::shader_compiler).
# The scene geometry front-end is the task/mesh meshlet pipeline (brief 03). The only remaining
# build-time-glslang GLSL shaders are the (out-of-scope) 2D grid pass.

# --- Slang sources (in-process compile + hot-reload) -------------------------------------------
slang_shaders = files(
    'ui_shader.slang',
    'text_shader.slang',
    'ui_background.slang',  # ui-dev sandbox flat gradient background (brief 05, milestone 0)
    'sky_shader.slang',
    'sky.slang',        # imported by sky_shader.slang, lighting.slang and ibl.slang
    'lighting.slang',   # shared lit-shading module (was missing from the install set pre-07)
    'ibl.slang',        # brief 07: sky->cubemap capture, GGX prefilter, SH projection, DFG LUT
    'froxel_cull.slang', # clustered light-binning compute (brief 02)
    'meshlet.slang',     # shared meshlet data model (imported by the mesh pipeline; brief 03)
    'meshlet_cull.slang',   # brief 04d M1: shared pure per-meshlet cull primitives (frustum/cone/HiZ/LOD)
    'meshlet_mesh.slang',   # task + mesh + fragment meshlet draw path (brief 03)
    'meshlet_shadow.slang', # task + mesh depth-only meshlet shadow path (brief 03)
    'meshlet_draw_cull.slang', # brief 04c draw phase: per-draw meshlet counts + LOD select
    'meshlet_expand.slang',    # brief 04c expansion: order-stable prefix scan + worklist fill
    'hiz_build.slang',      # HiZ depth-pyramid downsample compute (brief 03)
    'meshlet_reset.slang',  # per-frame stats/visibility reset compute (brief 03)
    'post.slang',           # brief 09: histogram auto-exposure + Karis bloom chain
    'gtao.slang',           # brief 09: half-res GTAO + bent normals + spatial denoise
)
install_data(slang_shaders, install_dir : 'include/string/shaders')

# --- Remaining GLSL shaders (build-time glslang) -----------------------------------------------
glsl_shaders = [
    'grid_2d_shader.vert',
    'grid_2d_shader.frag',
    'debug_line.vert',   # brief 06 immediate-mode debug lines (world-space line list)
    'debug_line.frag',
]

# Shared shader includes (#include "sky.glsl"). -I resolves them; listed as depend_files so a change
# rebuilds the shaders that pull them in.
shader_includes = files('sky.glsl')

shader_targets = []
foreach s : glsl_shaders
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
