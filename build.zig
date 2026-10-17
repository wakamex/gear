// gear on gesso and hearsay.
//   zig build                  build zig-out/bin/gear
//   zig build -Decho=true      with WebRTC's echo canceller in gs_voice, for loudspeakers
//   zig build test             run the tests (the end-to-end ones need the models; see README)
const std = @import("std");

const flags: []const []const u8 = &.{ "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };
const sources: []const []const u8 = &.{ "models.c", "listen.c", "hear.c", "say.c", "wav.c", "pipeline.c", "agent.c", "relay.c", "speakable.c", "window.c" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const echo = b.option(bool, "echo", "Give gs_voice WebRTC's echo canceller, for playing replies through loudspeakers") orelse false;
    const gesso = b.dependency("gesso", .{ .target = target, .optimize = optimize, .echo = echo }).artifact("gesso");
    const hearsay = b.dependency("hearsay", .{ .target = target, .optimize = optimize, .parakeet = true, .pockettts = true }).artifact("hearsay");

    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true, .strip = optimize != .debug });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = sources, .flags = flags });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"main.c"}, .flags = flags });
    mod.linkLibrary(gesso);
    mod.linkLibrary(hearsay);
    const exe = b.addExecutable(.{ .name = "gear", .root_module = mod });
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
