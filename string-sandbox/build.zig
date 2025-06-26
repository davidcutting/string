const std = @import("std");

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});

    const linux_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .linux,
    });
    const windows_target = b.resolveTargetQuery(.{
        .cpu_arch = .x86_64,
        .os_tag = .windows,
        .abi = .gnu,
    });

    build_target(b, "string_sandbox_linux", linux_target, optimize);
    build_target(b, "string_sandbox_windows", windows_target, optimize);
}

fn build_target(
    b: *std.Build,
    name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) void {
    const exe = b.addExecutable(.{
        .name = name,
        .target = target,
        .optimize = optimize,
        .linkage = .static,
    });
    exe.addCSourceFiles(.{
        .files = &.{
            "src/main.cpp",
        },
        .flags = &.{
            "-std=c++23",
        },
    });

    const spdlog_dep = b.dependency("spdlog", .{
        .target = target,
        .optimize = optimize,
    });
    exe.addIncludePath(spdlog_dep.path("include"));
    const vma_dep = b.dependency("vulkan_vma", .{
        .target = target,
        .optimize = optimize,
    });
    exe.addIncludePath(vma_dep.path("include"));
    const glfw3_dep = b.dependency("glfw3", .{
        .target = target,
        .optimize = optimize,
    });
    exe.addIncludePath(glfw3_dep.path("include"));
    const vulkan_headers_dep = b.dependency("vulkan_headers", .{
        .target = target,
        .optimize = optimize,
    });
    exe.addIncludePath(vulkan_headers_dep.path("include"));

    const string_lib_dep = b.dependency("string_engine", .{
        .target = target,
        .optimize = optimize,
    });
    exe.addIncludePath(string_lib_dep.path("include"));
    exe.linkLibrary(string_lib_dep.artifact("string-engine"));

    exe.linkLibC();
    exe.linkLibCpp();

    b.installArtifact(exe);

    // Run step
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step(name, "Run the app");
    run_step.dependOn(&run_cmd.step);
}
