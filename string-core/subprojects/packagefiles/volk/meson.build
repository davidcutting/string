project(
    'volk',
    'c',
    version : '1.4.350',
    license : 'MIT',
)

# Header-only: the engine compiles the volk implementation itself (src/gpu/driver.cpp defines
# VOLK_IMPLEMENTATION), so this wrap must NOT also compile volk.c — doing so would duplicate
# every volk* symbol at link time. We just expose volk.h and the Vulkan headers it needs.
# volk resolves the loader dynamically at runtime, so there is no libvulkan link.
vulkan_headers_dep = dependency('VulkanHeaders')

volk_dep = declare_dependency(
    include_directories : include_directories('.'),
    dependencies : vulkan_headers_dep,
)

meson.override_dependency('volk', volk_dep)
