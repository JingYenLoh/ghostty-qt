//! Project-private additions to Ghostty's config C API.
//!
//! CMake copies this file over `src/config/CApi.zig` in an isolated source
//! shadow and preserves the pinned implementation as `CApi_upstream.zig`.
//! Importing that file keeps every upstream export intact while the code below
//! adds the structured configuration boundary needed by the Qt frontend.

const std = @import("std");
const inputpkg = @import("../input.zig");
const Metrics = @import("../font/Metrics.zig");
const state = &@import("../global.zig").state;
const String = @import("../main_c.zig").String;

const Config = @import("Config.zig");
const Binding = inputpkg.Binding;

const MetricConfigDescriptor = struct {
    name: []const u8,
    key: Metrics.Key,
    exported: bool = true,
};

// Keep the pinned SharedGridSet.Key.init insertion sequence in one place.
// Unsupported sprite/icon metrics still enter the map because they can change
// its capacity and therefore the observable order of supported modifiers.
const metric_config_descriptors = [_]MetricConfigDescriptor{
    .{ .name = "adjust-cell-width", .key = .cell_width },
    .{ .name = "adjust-cell-height", .key = .cell_height },
    .{ .name = "adjust-font-baseline", .key = .cell_baseline },
    .{ .name = "adjust-underline-position", .key = .underline_position },
    .{ .name = "adjust-underline-thickness", .key = .underline_thickness },
    .{ .name = "adjust-strikethrough-position", .key = .strikethrough_position },
    .{ .name = "adjust-strikethrough-thickness", .key = .strikethrough_thickness },
    .{ .name = "adjust-overline-position", .key = .overline_position },
    .{ .name = "adjust-overline-thickness", .key = .overline_thickness },
    .{ .name = "adjust-cursor-thickness", .key = .cursor_thickness },
    .{ .name = "adjust-cursor-height", .key = .cursor_height },
    .{ .name = "adjust-box-thickness", .key = .box_thickness, .exported = false },
    .{ .name = "adjust-icon-height", .key = .icon_height, .exported = false },
};

comptime {
    _ = @import("CApi_upstream.zig");
}

/// Export one complete, finalized configuration generation as the
/// project-private JSON v1 schema. The returned allocation follows
/// ghostty_string_s ownership and is released by ghostty_string_free.
export fn ghostty_qt_config_json() String {
    return configJson() catch |err| {
        std.log.err("error exporting structured config as JSON err={}", .{err});
        return .empty;
    };
}

fn configJson() !String {
    var output: std.Io.Writer.Allocating = .init(state.alloc);
    errdefer output.deinit();

    var json: std.json.Stringify = .{ .writer = &output.writer };
    try json.beginObject();
    try json.objectField("version");
    try json.write(@as(u8, 1));

    {
        var config = try Config.load(state.alloc);
        defer config.deinit();

        try json.objectField("values");
        try writeValues(&json, &config);

        try json.objectField("keybindings");
        try writeKeybinds(&json, &config.keybind);
    }

    // Platform defaults let the frontend distinguish unsupported user
    // bindings from unsupported built-ins. Load them only after releasing the
    // current generation so the short-lived helper does not retain both.
    {
        var defaults = try Config.default(state.alloc);
        defer defaults.deinit();

        try json.objectField("default-keybindings");
        try writeKeybinds(&json, &defaults.keybind);
    }
    try json.endObject();

    return .fromSlice(try output.toOwnedSlice());
}

fn writeValues(json: *std.json.Stringify, config: *const Config) !void {
    try json.beginObject();

    try json.objectField("working-directory");
    try writeWorkingDirectory(json, config.@"working-directory" orelse return error.UnfinalizedConfig);
    inline for (.{
        .{ "font-family", &config.@"font-family" },
        .{ "font-family-bold", &config.@"font-family-bold" },
        .{ "font-family-italic", &config.@"font-family-italic" },
        .{ "font-family-bold-italic", &config.@"font-family-bold-italic" },
    }) |entry| {
        try json.objectField(entry[0]);
        try writeStringList(json, entry[1]);
    }
    try json.objectField("font-size");
    try json.write(config.@"font-size");
    inline for (.{
        .{ "font-style", config.@"font-style" },
        .{ "font-style-bold", config.@"font-style-bold" },
        .{ "font-style-italic", config.@"font-style-italic" },
        .{ "font-style-bold-italic", config.@"font-style-bold-italic" },
    }) |entry| {
        try json.objectField(entry[0]);
        try writeFontStyle(json, entry[1]);
    }
    inline for (metric_config_descriptors) |descriptor| {
        if (descriptor.exported) {
            try json.objectField(descriptor.name);
            try writeOptionalMetricModifier(
                json,
                @field(config, descriptor.name),
            );
        }
    }
    try json.objectField("metric-modifier-order");
    try writeMetricModifierOrder(json, config);
    try json.objectField("foreground");
    try writeRgb(json, config.foreground);
    try json.objectField("background");
    try writeRgb(json, config.background);
    try json.objectField("unfocused-split-opacity");
    try json.write(config.@"unfocused-split-opacity");
    try json.objectField("unfocused-split-fill");
    try writeOptionalRgb(json, config.@"unfocused-split-fill");
    try json.objectField("split-divider-color");
    try writeOptionalRgb(json, config.@"split-divider-color");
    try json.objectField("split-inherit-working-directory");
    try json.write(config.@"split-inherit-working-directory");
    try json.objectField("split-preserve-zoom");
    try json.write(config.@"split-preserve-zoom".navigation);
    try json.objectField("tab-inherit-working-directory");
    try json.write(config.@"tab-inherit-working-directory");
    try json.objectField("window-inherit-working-directory");
    try json.write(config.@"window-inherit-working-directory");
    try json.objectField("window-inherit-font-size");
    try json.write(config.@"window-inherit-font-size");
    try json.objectField("window-new-tab-position");
    try json.write(@tagName(config.@"window-new-tab-position"));
    try json.objectField("window-show-tab-bar");
    try json.write(@tagName(config.@"window-show-tab-bar"));
    try json.objectField("window-decoration");
    try json.write(@tagName(config.@"window-decoration"));
    try json.objectField("window-width");
    try json.write(config.@"window-width");
    try json.objectField("window-height");
    try json.write(config.@"window-height");
    try json.objectField("maximize");
    try json.write(config.maximize);
    try json.objectField("fullscreen");
    try json.write(@tagName(config.fullscreen));
    try json.objectField("palette");
    try json.beginArray();
    for (config.palette.value) |color| try writeRgb(json, color);
    try json.endArray();
    try json.objectField("selection-foreground");
    try writeOptionalTerminalColor(json, config.@"selection-foreground");
    try json.objectField("selection-background");
    try writeOptionalTerminalColor(json, config.@"selection-background");
    try json.objectField("search-foreground");
    try writeTerminalColor(json, config.@"search-foreground");
    try json.objectField("search-background");
    try writeTerminalColor(json, config.@"search-background");
    try json.objectField("search-selected-foreground");
    try writeTerminalColor(json, config.@"search-selected-foreground");
    try json.objectField("search-selected-background");
    try writeTerminalColor(json, config.@"search-selected-background");
    try json.objectField("cursor-color");
    try writeOptionalTerminalColor(json, config.@"cursor-color");
    try json.objectField("cursor-opacity");
    try json.write(config.@"cursor-opacity");
    try json.objectField("cursor-style");
    try json.write(@tagName(config.@"cursor-style"));
    try json.objectField("cursor-style-blink");
    if (config.@"cursor-style-blink") |blink| try json.write(blink) else try json.write(null);
    try json.objectField("cursor-text");
    try writeOptionalTerminalColor(json, config.@"cursor-text");
    try json.objectField("bold-color");
    try writeOptionalBoldColor(json, config.@"bold-color");
    try json.objectField("faint-opacity");
    try json.write(config.@"faint-opacity");
    try json.objectField("scrollback-limit");
    var scrollback_buf: [32]u8 = undefined;
    try json.write(try std.fmt.bufPrint(&scrollback_buf, "{d}", .{config.@"scrollback-limit"}));
    try json.objectField("scrollbar");
    try json.write(@tagName(config.scrollbar));
    try json.objectField("bell-features");
    try json.beginObject();
    inline for (.{
        "system",
        "audio",
        "attention",
        "title",
        "border",
    }) |feature| {
        try json.objectField(feature);
        try json.write(@field(config.@"bell-features", feature));
    }
    try json.endObject();
    try json.objectField("bell-audio-path");
    try writeOptionalConfigPath(json, config.@"bell-audio-path");
    try json.objectField("bell-audio-volume");
    try json.write(config.@"bell-audio-volume");
    try json.objectField("confirm-close-surface");
    try json.write(@tagName(config.@"confirm-close-surface"));
    try json.objectField("clipboard-trim-trailing-spaces");
    try json.write(config.@"clipboard-trim-trailing-spaces");
    try json.objectField("clipboard-paste-protection");
    try json.write(config.@"clipboard-paste-protection");
    try json.objectField("clipboard-paste-bracketed-safe");
    try json.write(config.@"clipboard-paste-bracketed-safe");
    try json.objectField("copy-on-select");
    try json.write(@tagName(config.@"copy-on-select"));
    try json.objectField("selection-clear-on-typing");
    try json.write(config.@"selection-clear-on-typing");
    try json.objectField("selection-clear-on-copy");
    try json.write(config.@"selection-clear-on-copy");
    try json.objectField("middle-click-action");
    try json.write(@tagName(config.@"middle-click-action"));
    try json.objectField("mouse-reporting");
    try json.write(config.@"mouse-reporting");
    try json.objectField("mouse-hide-while-typing");
    try json.write(config.@"mouse-hide-while-typing");
    try json.objectField("focus-follows-mouse");
    try json.write(config.@"focus-follows-mouse");
    try json.objectField("mouse-scroll-multiplier");
    try json.beginObject();
    try json.objectField("precision");
    try json.write(config.@"mouse-scroll-multiplier".precision);
    try json.objectField("discrete");
    try json.write(config.@"mouse-scroll-multiplier".discrete);
    try json.endObject();
    try json.objectField("link-url");
    try json.write(config.@"link-url");
    try json.objectField("link-previews");
    try json.write(@tagName(config.@"link-previews"));
    try json.objectField("config-file");
    try writeConfigFiles(json, &config.@"config-file");
    try json.objectField("quit-after-last-window-closed");
    try json.write(config.@"quit-after-last-window-closed");
    try json.objectField("quit-after-last-window-closed-delay");
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
    try json.objectField("resize-overlay-duration");
    // Match the pinned GTK frontend's truncation and c_uint saturation.
    try json.write(config.@"resize-overlay-duration".asMilliseconds());
    try json.objectField("gtk-single-instance");
    try json.write(@tagName(config.@"gtk-single-instance"));
    try json.endObject();
}

fn writeKeybinds(json: *std.json.Stringify, keybinds: *const Config.Keybinds) !void {
    var sequence: std.ArrayList(Binding.Trigger) = .empty;
    defer sequence.deinit(state.alloc);

    try json.beginObject();
    try json.objectField("root");
    try json.beginArray();
    try writeSet(json, state.alloc, &keybinds.set, &sequence);
    try json.endArray();

    try json.objectField("tables");
    try json.beginArray();
    var tables = keybinds.tables.iterator();
    while (tables.next()) |table| {
        try json.beginObject();
        try json.objectField("name");
        try json.write(table.key_ptr.*);
        try json.objectField("bindings");
        try json.beginArray();
        try writeSet(json, state.alloc, table.value_ptr, &sequence);
        try json.endArray();
        try json.endObject();
    }
    try json.endArray();
    try json.endObject();
}

fn writeWorkingDirectory(json: *std.json.Stringify, value: Config.WorkingDirectory) !void {
    switch (value) {
        .home, .inherit => try json.write(@tagName(value)),
        .path => |path| try json.write(path),
    }
}

fn writeStringList(json: *std.json.Stringify, value: *const Config.RepeatableString) !void {
    try json.beginArray();
    for (value.list.items) |entry| try json.write(entry);
    try json.endArray();
}

fn writeFontStyle(json: *std.json.Stringify, style: Config.FontStyle) !void {
    try json.beginObject();
    try json.objectField("kind");
    switch (style) {
        .default => try json.write("automatic"),
        .false => try json.write("disabled"),
        .name => |name| {
            try json.write("named");
            try json.objectField("name");
            try json.write(name);
        },
    }
    try json.endObject();
}

fn writeOptionalMetricModifier(json: *std.json.Stringify, modifier: anytype) !void {
    const value = modifier orelse {
        try json.write(null);
        return;
    };

    try json.beginObject();
    try json.objectField("kind");
    switch (value) {
        .absolute => |pixels| {
            try json.write("absolute");
            try json.objectField("value");
            try json.write(pixels);
        },
        .percent => |multiplier| {
            try json.write("percentage");
            try json.objectField("value");
            try json.write(multiplier);
        },
    }
    try json.endObject();
}

fn writeMetricModifierOrder(json: *std.json.Stringify, config: *const Config) !void {
    var modifiers: Metrics.ModifierSet = .{};
    defer modifiers.deinit(state.alloc);
    inline for (metric_config_descriptors) |descriptor| {
        if (@field(config, descriptor.name)) |modifier| {
            try modifiers.put(state.alloc, descriptor.key, modifier);
        }
    }

    try json.beginArray();
    var iterator = modifiers.iterator();
    while (iterator.next()) |entry| {
        inline for (metric_config_descriptors) |descriptor| {
            if (descriptor.exported and descriptor.key == entry.key_ptr.*) {
                try json.write(descriptor.name);
                break;
            }
        }
    }
    try json.endArray();
}

fn writeRgb(json: *std.json.Stringify, color: anytype) !void {
    var buf: [7]u8 = undefined;
    try json.write(try std.fmt.bufPrint(
        &buf,
        "#{x:0>2}{x:0>2}{x:0>2}",
        .{ color.r, color.g, color.b },
    ));
}

fn writeOptionalRgb(json: *std.json.Stringify, color: ?Config.Color) !void {
    if (color) |value| try writeRgb(json, value) else try json.write(null);
}

fn writeTerminalColor(json: *std.json.Stringify, color: Config.TerminalColor) !void {
    switch (color) {
        .color => |value| try writeRgb(json, value),
        .@"cell-foreground", .@"cell-background" => try json.write(@tagName(color)),
    }
}

fn writeOptionalTerminalColor(json: *std.json.Stringify, color: ?Config.TerminalColor) !void {
    if (color) |value| try writeTerminalColor(json, value) else try json.write(null);
}

fn writeOptionalBoldColor(json: *std.json.Stringify, color: ?Config.BoldColor) !void {
    if (color) |value| switch (value) {
        .color => |rgb| try writeRgb(json, rgb),
        .bright => try json.write("bright"),
    } else try json.write(null);
}

fn writeOptionalConfigPath(json: *std.json.Stringify, path: ?Config.Path) !void {
    const value = path orelse {
        try json.write(null);
        return;
    };
    const encoded, const optional = switch (value) {
        .optional => |optional| .{ optional, true },
        .required => |required| .{ required, false },
    };
    try json.beginObject();
    try json.objectField("path");
    try json.write(encoded);
    try json.objectField("optional");
    try json.write(optional);
    try json.endObject();
}

fn writeConfigFiles(json: *std.json.Stringify, paths: *const Config.RepeatablePath) !void {
    try json.beginArray();
    var buf: [std.fs.max_path_bytes + 1]u8 = undefined;
    for (paths.value.items) |path| {
        const value = switch (path) {
            .optional => |optional| try std.fmt.bufPrint(&buf, "?{s}", .{optional}),
            .required => |required| required,
        };
        try json.write(value);
    }
    try json.endArray();
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
