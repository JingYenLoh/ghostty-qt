//! Project-private additions to Ghostty's config C API.
//!
//! CMake copies this file over `src/config/CApi.zig` in an isolated source
//! shadow and preserves the pinned implementation as `CApi_upstream.zig`.
//! Importing that file keeps every upstream export intact while the code below
//! adds the structured configuration boundary needed by the Qt frontend.

const std = @import("std");
const inputpkg = @import("../input.zig");
const state = &@import("../global.zig").state;
const String = @import("../main_c.zig").String;

const Config = @import("Config.zig");
const Binding = inputpkg.Binding;

comptime {
    _ = @import("CApi_upstream.zig");
}

/// Export values that Ghostty's text formatter cannot represent losslessly as
/// the project-private JSON v4 schema. The returned allocation follows
/// ghostty_string_s ownership and is released by ghostty_string_free.
export fn ghostty_qt_config_json(defaults: bool) String {
    return configJson(defaults) catch |err| {
        std.log.err("error exporting structured config as JSON err={}", .{err});
        return .empty;
    };
}

fn configJson(defaults: bool) !String {
    var config = if (defaults)
        try Config.default(state.alloc)
    else
        try Config.load(state.alloc);
    defer config.deinit();

    var output: std.Io.Writer.Allocating = .init(state.alloc);
    errdefer output.deinit();

    var json: std.json.Stringify = .{ .writer = &output.writer };
    try json.beginObject();
    try json.objectField("version");
    try json.write(@as(u8, 4));

    try json.objectField("application");
    try json.beginObject();
    try json.objectField("quit-after-last-window-closed");
    try json.write(config.@"quit-after-last-window-closed");
    try json.objectField("quit-after-last-window-closed-delay-ms");
    if (config.@"quit-after-last-window-closed-delay") |duration| {
        // This is the exact conversion used by the pinned GTK frontend:
        // truncate sub-millisecond precision and saturate at c_uint.
        try json.write(duration.asMilliseconds());
    } else {
        try json.write(null);
    }
    try json.objectField("initial-window");
    try json.write(config.@"initial-window");
    try json.objectField("resize-overlay");
    try json.write(@tagName(config.@"resize-overlay"));
    try json.objectField("resize-overlay-position");
    try json.write(@tagName(config.@"resize-overlay-position"));
    try json.objectField("resize-overlay-duration-ms");
    // Match the pinned GTK frontend's truncation and c_uint saturation.
    try json.write(config.@"resize-overlay-duration".asMilliseconds());
    try json.objectField("gtk-single-instance");
    try json.write(@tagName(config.@"gtk-single-instance"));
    try json.endObject();

    var sequence: std.ArrayList(Binding.Trigger) = .empty;
    defer sequence.deinit(state.alloc);

    try json.objectField("keybindings");
    try json.beginObject();
    try json.objectField("root");
    try json.beginArray();
    try writeSet(&json, state.alloc, &config.keybind.set, &sequence);
    try json.endArray();

    try json.objectField("tables");
    try json.beginArray();
    var tables = config.keybind.tables.iterator();
    while (tables.next()) |table| {
        try json.beginObject();
        try json.objectField("name");
        try json.write(table.key_ptr.*);
        try json.objectField("bindings");
        try json.beginArray();
        try writeSet(&json, state.alloc, table.value_ptr, &sequence);
        try json.endArray();
        try json.endObject();
    }
    try json.endArray();
    try json.endObject();
    try json.endObject();

    return .fromSlice(try output.toOwnedSlice());
}

fn writeSet(
    json: *std.json.Stringify,
    alloc: std.mem.Allocator,
    set: *const Binding.Set,
    sequence: *std.ArrayList(Binding.Trigger),
) !void {
    var bindings = set.bindings.iterator();
    while (bindings.next()) |entry| {
        try sequence.append(alloc, entry.key_ptr.*);
        defer _ = sequence.pop();

        switch (entry.value_ptr.*) {
            .leader => |next| try writeSet(json, alloc, next, sequence),
            .leaf => |leaf| try writeBinding(
                json,
                alloc,
                sequence.items,
                &.{leaf.action},
                leaf.flags,
            ),
            .leaf_chained => |leaf| try writeBinding(
                json,
                alloc,
                sequence.items,
                leaf.actions.items,
                leaf.flags,
            ),
        }
    }
}

fn writeBinding(
    json: *std.json.Stringify,
    alloc: std.mem.Allocator,
    sequence: []const Binding.Trigger,
    actions: []const Binding.Action,
    flags: Binding.Flags,
) !void {
    try json.beginObject();

    try json.objectField("sequence");
    try json.beginArray();
    for (sequence) |trigger| try writeTrigger(json, trigger);
    try json.endArray();

    try json.objectField("actions");
    try json.beginArray();
    for (actions) |action| {
        var formatted: std.Io.Writer.Allocating = .init(alloc);
        defer formatted.deinit();
        try action.format(&formatted.writer);
        try json.write(formatted.written());
    }
    try json.endArray();

    try json.objectField("flags");
    try json.beginObject();
    try json.objectField("consumed");
    try json.write(flags.consumed);
    try json.objectField("all");
    try json.write(flags.all);
    try json.objectField("global");
    try json.write(flags.global);
    try json.objectField("performable");
    try json.write(flags.performable);
    try json.endObject();

    try json.endObject();
}

fn writeTrigger(json: *std.json.Stringify, trigger: Binding.Trigger) !void {
    try json.beginObject();
    switch (trigger.key) {
        .physical => |key| {
            try json.objectField("kind");
            try json.write("physical");
            try json.objectField("key");
            try json.write(@tagName(key));
        },
        .unicode => |codepoint| {
            try json.objectField("kind");
            try json.write("unicode");
            try json.objectField("codepoint");
            try json.write(@as(u32, codepoint));
        },
        .catch_all => {
            try json.objectField("kind");
            try json.write("catch_all");
        },
    }
    try json.objectField("mods");
    try json.write(modifierBits(trigger.mods));
    try json.endObject();
}

fn modifierBits(mods: inputpkg.Mods) u8 {
    var result: u8 = 0;
    if (mods.shift) result |= 1;
    if (mods.ctrl) result |= 2;
    if (mods.alt) result |= 4;
    if (mods.super) result |= 8;
    return result;
}
