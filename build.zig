// gear on gesso and hearsay.
//   zig build                  build zig-out/bin/gear
//   zig build -Decho=true      with WebRTC's echo canceller in gs_voice, for loudspeakers
//   zig build -Dgpu=none       CPU only; the default runs the models on the GPU where one is usable,
//                              and needs the glslc shader compiler to build (-Dglslc=PATH)
//   zig build test             run the tests (the end-to-end ones need the models; see README)
const std = @import("std");

const flags: []const []const u8 = &.{ "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };
const sources: []const []const u8 = &.{ "models.c", "listen.c", "hear.c", "say.c", "wav.c", "pipeline.c", "agent.c", "relay.c", "speakable.c", "window.c" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const echo = b.option(bool, "echo", "Give gs_voice WebRTC's echo canceller, for playing replies through loudspeakers") orelse false;
    const gesso = b.dependency("gesso", .{ .target = target, .optimize = optimize, .echo = echo }).artifact("gesso");
    const gpu = b.option(enum { none, vulkan }, "gpu", "Run the models on a GPU where one is usable, else on the CPU (default vulkan)") orelse .vulkan;
    const glslc = b.option([]const u8, "glslc", "The glslc shader compiler, for -Dgpu=vulkan (default: glslc on the PATH)") orelse "glslc";
    const hearsay = b.dependency("hearsay", .{ .target = target, .optimize = optimize, .parakeet = true, .pockettts = true, .gpu = gpu, .glslc = glslc }).artifact("hearsay");

    // The build's name, given by tools/publish-update; a build without one never updates itself.
    const build_name = b.option([]const u8, "build", "Name this a published build, which keeps itself up to date (tools/publish-update)");
    const app_flags = if (build_name) |name| std.mem.concat(b.allocator, []const u8, &.{ flags, &.{b.fmt("-DGEAR_BUILD=\"{s}\"", .{name})} }) catch @panic("OOM") else flags;

    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true, .strip = optimize != .debug });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = sources, .flags = app_flags });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"main.c"}, .flags = app_flags });
    mod.linkLibrary(gesso);
    mod.linkLibrary(hearsay);
    const exe = b.addExecutable(.{ .name = "gear", .root_module = mod });
    // The icon Explorer, the taskbar and the window show (assets/icon/update.sh makes it).
    if (target.result.os.tag == .windows) mod.addWin32ResourceFile(.{ .file = b.path("assets/icon/gear.rc") });
    b.installArtifact(exe);
    // The window's font, Clanker Mono (D2Coding's glyphs, under the SIL Open Font License), beside the program.
    b.installBinFile("assets/ClankerMono-Regular.ttf", "ClankerMono-Regular.ttf");
    b.installBinFile("assets/ClankerMono-LICENSE.txt", "ClankerMono-LICENSE.txt");

    const tests = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true });
    tests.addCSourceFiles(.{ .root = b.path("src"), .files = sources, .flags = flags });
    tests.addCSourceFiles(.{ .root = b.path("tests"), .files = &.{"test.c"}, .flags = flags });
    tests.addIncludePath(b.path("src"));
    tests.linkLibrary(gesso);
    tests.linkLibrary(hearsay);
    const run = b.addRunArtifact(b.addExecutable(.{ .name = "gear-tests", .root_module = tests }));
    run.addDirectoryArg(b.path("tests"));
    // The models folder for the end-to-end tests: GEAR_MODELS, or the tests skip them.
    if (b.graph.environ_map.get("GEAR_MODELS")) |dir| run.setEnvironmentVariable("GEAR_MODELS", dir);
    // An agent kept for tests, for a real round trip through Wakterm: GEAR_AGENT, or the test skips it.
    if (b.graph.environ_map.get("GEAR_AGENT")) |name| run.setEnvironmentVariable("GEAR_AGENT", name);
    b.step("test", "Run the tests").dependOn(&run.step);
}
