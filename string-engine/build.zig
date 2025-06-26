const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const lib = b.addLibrary(.{
        .name = "string-engine",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libcpp = true,
        }),
    });

    lib.linkLibCpp();

    const spdlog_dep = b.dependency("spdlog", .{
        .target = target,
        .optimize = optimize,
    });
    lib.addIncludePath(spdlog_dep.path("include"));
    const vma_dep = b.dependency("vulkan_vma", .{
        .target = target,
        .optimize = optimize,
    });
    lib.addIncludePath(vma_dep.path("include"));
    const glfw3_dep = b.dependency("glfw3", .{
        .target = target,
        .optimize = optimize,
    });
    lib.addIncludePath(glfw3_dep.path("include"));
    const vulkan_headers_dep = b.dependency("vulkan_headers", .{
        .target = target,
        .optimize = optimize,
    });
    lib.addIncludePath(vulkan_headers_dep.path("include"));

    lib.linkSystemLibrary2("glm", .{
        .preferred_link_mode = .static,
        .use_pkg_config = .yes,
    });
    lib.linkSystemLibrary2("entt", .{
        .preferred_link_mode = .static,
        .use_pkg_config = .yes,
    });

    lib.addIncludePath(b.path("include"));
    lib.addCSourceFiles(.{ .root = b.path("src"), .files = &.{
        "application.cpp",
        "renderer.cpp",
        "window.cpp",
        "device.cpp",
        "allocator.cpp",
        "swapchain.cpp",
        "vulkan_utils.cpp",
        "pipeline_builder.cpp",
        "pipelines/pipeline_2d.cpp",
        "pipelines/pipeline_3d.cpp",
        "pipelines/pipeline_grid_2d.cpp",
        "pipelines/pipeline_grid_3d.cpp",
        "pipelines/hello_slang_pipeline.cpp",
        "core/logger.cpp",
    }, .flags = &.{
        "-std=c++23",
    } });

    lib.installHeadersDirectory(b.path("include"), "", .{});
    b.installArtifact(lib);
}
