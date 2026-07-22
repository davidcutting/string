# Slang shader sources are installed as-is; the engine compiles them in-process at startup (and
# recompiles on save for hot-reload) via string::gpu::shader_compiler, replacing the build-time
# glslang path. Only the composite resolve is a built-in engine shader; the scene passes' shaders
# live with those passes in the application (sandbox/shaders).
slang_shaders = files(
    'composite.slang',
)

install_data(slang_shaders, install_dir : 'include/string/shaders')
