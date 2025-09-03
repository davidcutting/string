const std = @import("std");
const zcc = @import("compile_commands");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe = b.addExecutable(.{
        .name = "string-engine",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .link_libcpp = true,
        }),
        .pic = true,
        .strip = true,
    });

    exe.linkLibC();
    exe.linkLibCpp();

    const volk_dep = b.dependency("volk", .{
        .target = target,
        .optimize = optimize,
        .linkage = .static,
    });
    exe.addIncludePath(volk_dep.path("."));

    const vulkan_header_dep = b.dependency("vulkan_headers", .{
        .target = target,
        .optimize = optimize,
        .linkage = .static,
    });
    exe.addIncludePath(vulkan_header_dep.path("include"));

    const vma_dep = b.dependency("vulkan_vma", .{
        .target = target,
        .optimize = optimize,
        .linkage = .static,
    });
    exe.addIncludePath(vma_dep.path("include"));

    const entt_dep = b.dependency("entt", .{
        .target = target,
        .optimize = optimize,
        .linkage = .static,
    });
    exe.addIncludePath(entt_dep.path("src"));

    exe.linkSystemLibrary2("glm", .{
        .preferred_link_mode = .static,
        .use_pkg_config = .yes,
    });

    exe.addIncludePath(b.path("include"));
    exe.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{
            "test_application.cpp",
            "application.cpp",
            "vulkan/renderer.cpp",
            "vulkan/device.cpp",
            "vulkan/allocator.cpp",
            "vulkan/swapchain.cpp",
            "vulkan/vulkan_utils.cpp",
            "vulkan/pipeline_builder.cpp",
            "vulkan/pipelines/pipeline_2d.cpp",
            "vulkan/pipelines/pipeline_3d.cpp",
            "vulkan/pipelines/pipeline_grid_2d.cpp",
            "vulkan/pipelines/pipeline_grid_3d.cpp",
            "vulkan/pipelines/hello_slang_pipeline.cpp",
        },
        .flags = &.{
            "-std=c++23",
        },
    });

    switch (target.result.os.tag) {
        .windows => {
            exe.addCSourceFiles(.{
                .files = &.{
                    "src/platform/sdl_window.cpp",
                },
                .flags = &.{
                    "-std=c++23",
                },
            });

            const sdl_dep = b.dependency("sdl", .{
                .target = target,
                .optimize = optimize,
                .preferred_linkage = .static,
                .install_build_config_h = true,
            });
            exe.linkLibrary(sdl_dep.artifact("SDL3"));
        },
        .linux => {
            exe.addCSourceFiles(.{
                .files = &.{
                    "src/platform/sdl_window.cpp",
                    // "src/platform/glfw_window.cpp",
                    // "src/platform/wayland/client.cpp",
                    // "src/platform/wayland/window.cpp",
                },
                .flags = &.{
                    "-std=c++23",
                },
            });

            const sdl_dep = b.dependency("sdl", .{
                .target = target,
                .optimize = optimize,
                .preferred_linkage = .static,
                .install_build_config_h = true,
            });
            exe.linkLibrary(sdl_dep.artifact("SDL3"));

            // const glfw_dep = b.dependency("glfw3", .{
            //     .target = target,
            //     .optimize = optimize,
            //     .native = false,
            //     .wayland = true,
            // });
            // exe.addIncludePath(glfw_dep.path("include"));
            // exe.linkLibrary(glfw_dep.artifact("glfw"));

            // exe.linkSystemLibrary2("glfw3", .{
            //     .preferred_link_mode = .static,
            //     .use_pkg_config = .yes,
            // });

            // const wayland_protocols_dep = b.dependency("wayland_protocols", .{
            //     .target = target,
            //     .optimize = optimize,
            //     .linkage = .static,
            // });
            // const wayland_dep = b.dependency("wayland", .{
            //     .target = target,
            //     .optimize = optimize,
            //     .linkage = .static,
            // });
            // const wayland_client = wayland_dep.artifact("wayland-client");
            // exe.linkLibrary(wayland_client);

            // const scanner = wayland_dep.artifact("wayland-scanner");
            // const xdg_xml = wayland_protocols_dep.path("stable/xdg-shell/xdg-shell.xml");

            // // Create header
            // const gen_header = b.addRunArtifact(scanner);
            // gen_header.addArgs(&.{"client-header"});
            // gen_header.addFileArg(xdg_xml);
            // const xdg_header = gen_header.addOutputFileArg("xdg-shell-client-protocol.h");
            // b.default_step.dependOn(&gen_header.step);

            // // Create source
            // const gen_source = b.addRunArtifact(scanner);
            // gen_source.addArgs(&.{"private-code"});
            // gen_source.addFileArg(xdg_xml);
            // const xdg_source = gen_source.addOutputFileArg("xdg-shell-protocol.c");
            // b.default_step.dependOn(&gen_source.step);

            // exe.addIncludePath(xdg_header.dirname());
            // exe.addCSourceFile(.{
            //     .file = xdg_source,
            //     .flags = &.{},
            // });
        },
        else => {
            @panic("Unsupported target!");
        },
    }

    exe.installHeadersDirectory(b.path("include"), "", .{});

    var targets = std.ArrayList(*std.Build.Step.Compile).init(b.allocator);
    targets.append(exe) catch @panic("OOM");
    const zcc_step = zcc.createStep(b, "gen-cc", targets.toOwnedSlice() catch @panic("OOM"));
    zcc_step.dependOn(&exe.step);

    // Run step
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    b.installArtifact(exe);
}
