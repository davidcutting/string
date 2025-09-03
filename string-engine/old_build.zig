const std = @import("std");
const zcc = @import("compile_commands");

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    const native_target = b.standardTargetOptions(.{});
    const linux_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .linux,
        .abi = .musl,
    });
    const windows_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .windows,
    });

    const native_exe = build_target(b, "string-native", native_target, optimize);
    const linux_exe = build_target(b, "string-linux", linux_target, optimize);
    const windows_exe = build_target(b, "string-win", windows_target, optimize);

    // Install compile commands
    var targets = std.ArrayList(*std.Build.Step.Compile).init(b.allocator);
    targets.append(native_exe) catch @panic("OOM");
    targets.append(linux_exe) catch @panic("OOM");
    targets.append(windows_exe) catch @panic("OOM");
    const zcc_step = zcc.createStep(b, "gen-cc", targets.toOwnedSlice() catch @panic("OOM"));
    zcc_step.dependOn(&native_exe.step);
    zcc_step.dependOn(&linux_exe.step);
    zcc_step.dependOn(&windows_exe.step);

    // Run step
    const run_cmd = b.addRunArtifact(native_exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);
}

fn build_target(
    b: *std.Build,
    comptime name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    // Executable
    const exe = b.addExecutable(.{
        .root_module = b.addModule(name, .{
            .link_libc = true,
            .link_libcpp = true,
            .target = target,
            .optimize = optimize,
        }),
        .name = name,
        .optimize = optimize,
        .pic = true,
        .strip = true,
    });
    exe.addCSourceFiles(.{
        .files = &.{
            "src/main.cpp",
            "src/vulkan/instance.cpp",
            "src/vulkan/device.cpp",
            "src/vulkan/presenter.cpp",
        },
        .flags = &.{
            "-std=c++23",
        },
    });
    exe.addIncludePath(b.path("include"));

    const vulkan_header_dep = b.dependency("vulkan_headers", .{
        .target = target,
        .optimize = optimize,
        .linkage = .static,
    });
    exe.addIncludePath(vulkan_header_dep.path("include"));

    const volk_dep = b.dependency("volk", .{
        .target = target,
        .optimize = optimize,
        .linkage = .static,
    });
    exe.addIncludePath(volk_dep.path("."));

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

    switch (target.result.os.tag) {
        .windows => {
            // exe.linkage = .dynamic;
            exe.addCSourceFiles(.{
                .files = &.{
                    "src/platform/windows_platform.cpp",
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
            exe.addIncludePath(sdl_dep.path("include"));
            exe.linkLibrary(sdl_dep.artifact("SDL3"));
        },
        .linux => {
            exe.addCSourceFiles(.{
                .files = &.{
                    "src/platform/client.cpp",
                    "src/platform/window.cpp",
                    "src/platform/linux_platform.cpp",
                },
                .flags = &.{
                    "-std=c++23",
                },
            });

            exe.linkSystemLibrary("vulkan");

            const wayland_protocols_dep = b.dependency("wayland_protocols", .{
                .target = target,
                .optimize = optimize,
                .linkage = .static,
            });
            const wayland_dep = b.dependency("wayland", .{
                .target = target,
                .optimize = optimize,
                .linkage = .static,
            });
            const wayland_client = wayland_dep.artifact("wayland-client");
            exe.linkLibrary(wayland_client);

            const scanner = wayland_dep.artifact("wayland-scanner");
            const xdg_xml = wayland_protocols_dep.path("stable/xdg-shell/xdg-shell.xml");

            // Create header
            const gen_header = b.addRunArtifact(scanner);
            gen_header.addArgs(&.{"client-header"});
            gen_header.addFileArg(xdg_xml);
            const xdg_header = gen_header.addOutputFileArg("xdg-shell-client-protocol.h");
            b.default_step.dependOn(&gen_header.step);

            // Create source
            const gen_source = b.addRunArtifact(scanner);
            gen_source.addArgs(&.{"private-code"});
            gen_source.addFileArg(xdg_xml);
            const xdg_source = gen_source.addOutputFileArg("xdg-shell-protocol.c");
            b.default_step.dependOn(&gen_source.step);

            exe.addIncludePath(xdg_header.dirname());
            exe.addCSourceFile(.{
                .file = xdg_source,
                .flags = &.{},
            });
        },
        else => {
            @panic("Unsupported target!");
        },
    }

    // Install
    exe.installHeadersDirectory(b.path("include"), "", .{});
    b.installArtifact(exe);

    return exe;
}
