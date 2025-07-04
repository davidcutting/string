const std = @import("std");
const zcc = @import("compile_commands");

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.standardTargetOptions(.{});

    const name = "voronoi";

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
        .linkage = .dynamic,
    });
    exe.addCSourceFiles(.{
        .files = &.{
            "src/main.cpp",
        },
        .flags = &.{
            "-std=c++23",
        },
    });
    exe.addIncludePath(b.path("include"));

    exe.linkLibC();
    exe.linkLibCpp();

    // Install compile commands
    var targets = std.ArrayList(*std.Build.Step.Compile).init(b.allocator);
    targets.append(exe) catch @panic("OOM");
    zcc.createStep(b, "generate_compile_commands", targets.toOwnedSlice() catch @panic("OOM"));

    // Install
    exe.installHeadersDirectory(b.path("include"), "", .{});
    b.installArtifact(exe);

    // Run command
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);
}
