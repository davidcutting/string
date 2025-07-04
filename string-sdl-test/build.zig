const std = @import("std");
const zcc = @import("compile_commands");

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    _ = b.standardTargetOptions(.{});
    const linux_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .linux,
        .abi = .musl,
    });
    const windows_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .windows,
        .abi = .gnu,
    });

    const linux_exe = build_target(b, "string-linux", linux_target, optimize);
    _ = build_target(b, "string-win", windows_target, optimize);

    // Install compile commands
    var targets = std.ArrayList(*std.Build.Step.Compile).init(b.allocator);
    targets.append(linux_exe) catch @panic("OOM");
    const zcc_step = zcc.createStep(b, "gen-cc", targets.toOwnedSlice() catch @panic("OOM"));
    zcc_step.dependOn(&linux_exe.step);

    // Run step
    const run_cmd = b.addRunArtifact(linux_exe);
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
    });
    exe.addCSourceFiles(.{
        .files = &.{
            "src/main.cpp",
            "src/platform/window.cpp",
            "src/platform/platform.cpp",
        },
        .flags = &.{
            "-std=c++23",
        },
    });
    exe.addIncludePath(b.path("include"));

    const entt_dep = b.dependency("entt", .{
        .target = target,
        .optimize = optimize,
    });
    exe.addIncludePath(entt_dep.path("src"));

    switch (target.result.os.tag) {
        .windows => {
            exe.linkage = .dynamic;
            exe.addCSourceFiles(.{
                .files = &.{
                    // "src/sdl_test.cpp",
                    // "src/sdl_test_2.cpp",
                    "src/platform/impl/sdl_window.cpp",
                    "src/platform/impl/sdl_platform.cpp",
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
            exe.linkage = .static;
            exe.addCSourceFiles(.{
                .files = &.{
                    "src/platform/impl/glfw_window.cpp",
                    "src/platform/impl/glfw_platform.cpp",
                },
                .flags = &.{
                    "-std=c++23",
                },
            });
            const glfw_dep = b.dependency("glfw", .{
                .target = target,
                .optimize = optimize,
                .include_src = true,
                // .x11 = false,
                .wayland = true,
            });
            exe.linkLibrary(glfw_dep.artifact("glfw"));
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
