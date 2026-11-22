// gear on gesso and hearsay.
//   zig build                  build zig-out/bin/gear
//   zig build -Decho=true      with WebRTC's echo canceller in gs_voice, for loudspeakers
//   zig build -Dgpu=none       CPU only; the default runs the models on the GPU where one is usable,
//                              and needs the glslc shader compiler to build (-Dglslc=PATH)
//   zig build test             run the tests (the end-to-end ones need the models; see README)
//   zig build -Dtarget=aarch64-linux-android.29 -Dandroid_ndk=PATH   the phone app's libmain.so (tools/android-apk packs it)
const std = @import("std");
const gesso_build = @import("gesso");

const flags: []const []const u8 = &.{ "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };
const sources: []const []const u8 = &.{ "models.c", "listen.c", "hear.c", "say.c", "serve.c", "wav.c", "pipeline.c", "agent.c", "record.c", "speakable.c", "pronounce.c", "panetone.c", "speech.c", "transcript.c", "vad.c", "window.c" };

// The phone app's own sources (src/phone), and what it needs of mbedTLS: its cryptography, for libssh2
// and the phone's key, without TLS and certificates.
const phone_sources: []const []const u8 = &.{ "main.c", "key.c", "link.c", "rich.c" };
const mbedtls_sources: []const []const u8 = &.{ "aes.c", "aesce.c", "aesni.c", "aria.c", "asn1parse.c", "asn1write.c", "base64.c", "bignum.c", "bignum_core.c", "bignum_mod.c", "bignum_mod_raw.c", "block_cipher.c", "camellia.c", "ccm.c", "chacha20.c", "chachapoly.c", "cipher.c", "cipher_wrap.c", "cmac.c", "constant_time.c", "ctr_drbg.c", "des.c", "dhm.c", "ecdh.c", "ecdsa.c", "ecjpake.c", "ecp.c", "ecp_curves.c", "ecp_curves_new.c", "entropy.c", "entropy_poll.c", "error.c", "gcm.c", "hkdf.c", "hmac_drbg.c", "lmots.c", "lms.c", "md5.c", "md.c", "memory_buffer_alloc.c", "nist_kw.c", "oid.c", "padlock.c", "pem.c", "pk.c", "pkcs12.c", "pkcs5.c", "pk_ecc.c", "pkparse.c", "pk_wrap.c", "pkwrite.c", "platform.c", "platform_util.c", "poly1305.c", "psa_crypto_aead.c", "psa_crypto.c", "psa_crypto_cipher.c", "psa_crypto_client.c", "psa_crypto_driver_wrappers_no_static.c", "psa_crypto_ecp.c", "psa_crypto_ffdh.c", "psa_crypto_hash.c", "psa_crypto_mac.c", "psa_crypto_pake.c", "psa_crypto_rsa.c", "psa_crypto_se.c", "psa_crypto_slot_management.c", "psa_crypto_storage.c", "psa_its_file.c", "psa_util.c", "ripemd160.c", "rsa_alt_helpers.c", "rsa.c", "sha1.c", "sha256.c", "sha3.c", "sha512.c", "threading.c", "timing.c", "version.c", "version_features.c" };
const libssh2_sources: []const []const u8 = &.{ "agent.c", "bcrypt_pbkdf.c", "blowfish.c", "chacha.c", "channel.c", "cipher-chachapoly.c", "comp.c", "crypt.c", "crypto.c", "global.c", "hostkey.c", "keepalive.c", "kex.c", "knownhost.c", "mac.c", "misc.c", "packet.c", "pem.c", "poly1305.c", "publickey.c", "scp.c", "session.c", "sftp.c", "transport.c", "userauth.c", "userauth_kbd_packet.c", "version.c" };

// libssh2 over mbedTLS, as one static library, with their headers for the app.
fn sshLibrary(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) *std.Build.Step.Compile {
    const mbedtls = b.dependency("mbedtls", .{});
    const libssh2 = b.dependency("libssh2", .{});
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .pic = true });
    mod.addIncludePath(mbedtls.path("include"));
    mod.addIncludePath(mbedtls.path("library"));
    mod.addCSourceFiles(.{ .root = mbedtls.path("library"), .files = mbedtls_sources, .flags = &.{ "-std=c99", "-w" } });
    mod.addIncludePath(libssh2.path("include"));
    mod.addIncludePath(libssh2.path("src"));
    // (on Windows, libssh2's own Winsock code, with none of the POSIX headers)
    const windows = target.result.os.tag == .windows;
    mod.addCSourceFiles(.{ .root = libssh2.path("src"), .files = libssh2_sources, .flags = if (windows) &.{
        "-std=c99", "-w", "-DLIBSSH2_MBEDTLS", "-DHAVE_INTTYPES_H", "-DHAVE_STRTOLL", "-DHAVE_SNPRINTF", "-DWIN32_LEAN_AND_MEAN",
    } else &.{
        "-std=c99", "-w", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-DLIBSSH2_MBEDTLS", "-DHAVE_UNISTD_H", "-DHAVE_INTTYPES_H", "-DHAVE_SYS_SELECT_H",
        "-DHAVE_SYS_UIO_H", "-DHAVE_SYS_SOCKET_H", "-DHAVE_SYS_IOCTL_H", "-DHAVE_SYS_TIME_H", "-DHAVE_SYS_UN_H", "-DHAVE_SYS_PARAM_H", "-DHAVE_ARPA_INET_H",
        "-DHAVE_NETINET_IN_H", "-DHAVE_O_NONBLOCK", "-DHAVE_POLL", "-DHAVE_GETTIMEOFDAY", "-DHAVE_STRTOLL", "-DHAVE_SNPRINTF",
    } });
    if (windows) for ([_][]const u8{ "ws2_32", "bcrypt", "advapi32" }) |l| mod.linkSystemLibrary(l, .{});
    const lib = b.addLibrary(.{ .linkage = .static, .name = "ssh", .root_module = mod });
    lib.installHeadersDirectory(libssh2.path("include"), "", .{});
    lib.installHeadersDirectory(mbedtls.path("include"), "", .{});
    return lib;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    // On Android only the phone app: libmain.so, which SDL's activity loads and runs main() from.
    if (target.result.abi.isAndroid()) {
        const android_ndk = b.option([]const u8, "android_ndk", "Android NDK root, for building for Android") orelse @panic("-Dandroid_ndk=PATH is needed to build for Android");
        const gesso_dep = b.dependency("gesso", .{ .target = target, .optimize = optimize, .android_ndk = android_ndk });
        const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .strip = optimize != .debug, .pic = true });
        // The commit it was built from, given by tools/android-apk, which the app shows in its top line.
        const build_name = b.option([]const u8, "build", "The commit this is built from (tools/android-apk)");
        const phone_flags = if (build_name) |name| std.mem.concat(b.allocator, []const u8, &.{ flags, &.{b.fmt("-DGEAR_BUILD=\"{s}\"", .{name})} }) catch @panic("OOM") else flags;
        mod.addCSourceFiles(.{ .root = b.path("src/phone"), .files = phone_sources, .flags = phone_flags });
        mod.linkLibrary(gesso_dep.artifact("gesso"));
        const ssh = sshLibrary(b, target, optimize);
        gesso_build.useNdk(ssh, gesso_dep);  // (the NDK's C library, which libssh2 and mbedTLS need as SDL does)
        mod.linkLibrary(ssh);
        for ([_][]const u8{ "log", "dl", "m" }) |lib| mod.linkSystemLibrary(lib, .{});
        const lib = b.addLibrary(.{ .linkage = .dynamic, .name = "main", .root_module = mod });
        gesso_build.useNdk(lib, gesso_dep);
        b.installArtifact(lib);
        b.installDirectory(.{ .source_dir = gesso_dep.namedLazyPath("android_java"), .install_dir = .prefix, .install_subdir = "java" });
        return;
    }
    const echo = b.option(bool, "echo", "Give gs_voice WebRTC's echo canceller, for playing replies through loudspeakers") orelse false;
    const gesso = b.dependency("gesso", .{ .target = target, .optimize = optimize, .echo = echo }).artifact("gesso");
    const gpu = b.option(enum { none, vulkan }, "gpu", "Run the models on a GPU where one is usable, else on the CPU (default vulkan)") orelse .vulkan;
    const glslc = b.option([]const u8, "glslc", "The glslc shader compiler, for -Dgpu=vulkan (default: glslc on the PATH)") orelse "glslc";
    const hearsay = b.dependency("hearsay", .{ .target = target, .optimize = optimize, .parakeet = true, .pockettts = true, .silero = true, .gpu = gpu, .glslc = glslc }).artifact("hearsay");

    // The build's name, given by tools/publish-update; a build without one never updates itself.
    const build_name = b.option([]const u8, "build", "Name this a published build, which keeps itself up to date (tools/publish-update)");
    const app_flags = if (build_name) |name| std.mem.concat(b.allocator, []const u8, &.{ flags, &.{b.fmt("-DGEAR_BUILD=\"{s}\"", .{name})} }) catch @panic("OOM") else flags;

    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true, .strip = optimize != .debug });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = sources, .flags = app_flags });
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"main.c"}, .flags = app_flags });
    mod.linkLibrary(gesso);
    mod.linkLibrary(hearsay);
    const exe = b.addExecutable(.{ .name = "gear", .root_module = mod });
    // A windowed program on Windows, so starting it from a shortcut or the taskbar opens no console;
    // run from a terminal, it writes to that terminal's console (main.c).
    if (target.result.os.tag == .windows) exe.subsystem = .windows;
    // The icon Explorer, the taskbar and the window show (assets/icon/update.sh makes it).
    if (target.result.os.tag == .windows) mod.addWin32ResourceFile(.{ .file = b.path("assets/icon/gear.rc") });
    b.installArtifact(exe);
    // The window's font, Clanker Mono (D2Coding's glyphs, under the SIL Open Font License), beside the program.
    b.installBinFile("assets/ClankerMono-Regular.ttf", "ClankerMono-Regular.ttf");
    b.installBinFile("assets/ClankerMono-LICENSE.txt", "ClankerMono-LICENSE.txt");

    // The phone app built for this computer too: on Windows the desktop's gear, a client of the hub as
    // the phone is; elsewhere, to try it against a hub without a phone.
    {
        const phone = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .strip = optimize != .debug });
        phone.addCSourceFiles(.{ .root = b.path("src/phone"), .files = phone_sources, .flags = app_flags });
        phone.linkLibrary(gesso);
        phone.linkLibrary(sshLibrary(b, target, optimize));
        if (target.result.os.tag == .windows) phone.linkSystemLibrary("ws2_32", .{});
        const phone_exe = b.addExecutable(.{ .name = "gear-phone", .root_module = phone });
        if (target.result.os.tag == .windows) {
            phone_exe.subsystem = .windows;
            phone.addWin32ResourceFile(.{ .file = b.path("assets/icon/gear.rc") });
        }
        b.installArtifact(phone_exe);
        // (its characters' frames, beside it, where it looks for them as the Android app does in its assets)
        b.installDirectory(.{ .source_dir = b.path("assets/avatars"), .install_dir = .bin, .install_subdir = "avatars" });
    }

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
