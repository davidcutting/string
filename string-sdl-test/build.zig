const std = @import("std");
const zcc = @import("compile_commands");

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{
        .preferred_optimize_mode = .ReleaseSafe,
    });
    // const local_target = b.standardTargetOptions(.{});
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

    // const local_exe = build_target(b, "string-local", local_target, optimize, false);
    const linux_exe = build_target(b, "string-linux", linux_target, optimize, true);
    _ = build_target(b, "string-win", windows_target, optimize, false);

    // Install compile commands
    var targets = std.ArrayList(*std.Build.Step.Compile).init(b.allocator);
    targets.append(linux_exe) catch @panic("OOM");
    const zcc_step = zcc.createStep(b, "gen-cc", targets.toOwnedSlice() catch @panic("OOM"));
    zcc_step.dependOn(&linux_exe.step);
}

fn build_target(
    b: *std.Build,
    comptime name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    comptime is_static: bool,
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
        .linkage = if (is_static) .static else .dynamic,
    });
    exe.addCSourceFiles(.{
        .files = &.{
            "src/main.cpp",
            // "src/sdl_test.cpp",
            // "src/sdl_test_2.cpp",
            "src/platform/window.cpp",
            "src/platform/platform.cpp",
            // "src/platform/impl/sdl_window.cpp",
            // "src/platform/impl/sdl_platform.cpp",
            "src/platform/impl/glfw_window.cpp",
            "src/platform/impl/glfw_platform.cpp",
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

    // const sdl_dep = b.dependency("sdl", .{
    //     .target = target,
    //     .optimize = optimize,
    //     .preferred_linkage = .static,
    //     .install_build_config_h = true,
    // });
    // exe.addIncludePath(sdl_dep.path("include"));
    // exe.linkLibrary(sdl_dep.artifact("SDL3"));

    const glfw_dep = b.dependency("glfw", .{
        .target = target,
        .optimize = optimize,
        .include_src = true,
        // .x11 = false,
        .wayland = true,
    });
    exe.linkLibrary(glfw_dep.artifact("glfw"));

    // Install
    exe.installHeadersDirectory(b.path("include"), "", .{});
    b.installArtifact(exe);

    // Run command
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step(name ++ "-run", "Run the app");
    run_step.dependOn(&run_cmd.step);

    return exe;
}
