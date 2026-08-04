const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const ghostty_url_path = b.option(
        []const u8,
        "ghostty-url-path",
        "Absolute path to the pinned Ghostty src/config/url.zig",
    ) orelse @panic("-Dghostty-url-path is required");

    const oniguruma = b.dependency("oniguruma", .{
        .target = target,
        .optimize = optimize,
    });
    const url_module = b.createModule(.{
        .root_source_file = .{ .cwd_relative = ghostty_url_path },
        .target = target,
        .optimize = optimize,
    });
    url_module.addImport("oniguruma", oniguruma.module("oniguruma"));

    const library_module = b.createModule(.{
        .root_source_file = b.path("matcher.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    library_module.addImport("ghostty_url", url_module);
    library_module.addImport("oniguruma", oniguruma.module("oniguruma"));

    const library = b.addLibrary(.{
        .name = "ghostty-qt-link-matcher",
        .root_module = library_module,
        .linkage = .static,
    });
    // ReleaseSafe emits Zig stack-probe/runtime helpers that a C++ link does
    // not otherwise provide. Keep the static ABI self-contained in every
    // configured optimization mode, matching Ghostty's own lib-vt build.
    library.bundle_compiler_rt = true;
    library.root_module.linkLibrary(oniguruma.artifact("oniguruma"));
    b.installArtifact(library);
    // Zig does not fold a linked static archive into another static archive.
    // Stage the pinned engine beside the matcher so CMake can preserve link
    // order without reaching into Zig's content-addressed cache.
    b.installArtifact(oniguruma.artifact("oniguruma"));

    const test_module = b.createModule(.{
        .root_source_file = b.path("matcher.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    test_module.addImport("ghostty_url", url_module);
    test_module.addImport("oniguruma", oniguruma.module("oniguruma"));
    const tests = b.addTest(.{
        .name = "ghostty-qt-link-matcher-tests",
        .root_module = test_module,
    });
    tests.root_module.linkLibrary(oniguruma.artifact("oniguruma"));

    const run_tests = b.addRunArtifact(tests);
    const corpus_tests = b.addTest(.{
        .name = "ghostty-url-corpus-tests",
        .root_module = url_module,
    });
    corpus_tests.root_module.linkLibrary(oniguruma.artifact("oniguruma"));
    const run_corpus_tests = b.addRunArtifact(corpus_tests);
    const test_step = b.step(
        "test",
        "Run the matcher tests and Ghostty's imported URL/path corpus",
    );
    test_step.dependOn(&run_tests.step);
    test_step.dependOn(&run_corpus_tests.step);
}
