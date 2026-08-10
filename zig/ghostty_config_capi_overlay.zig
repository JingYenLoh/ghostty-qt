//! Project-private additions to Ghostty's config C API.
//!
//! CMake copies this file over `src/config/CApi.zig` in an isolated source
//! shadow and preserves the pinned implementation as `CApi_upstream.zig`.
//! Importing that file keeps every upstream export intact while the code below
//! adds the structured configuration boundary needed by the Qt frontend.

const std = @import("std");
const cli = @import("../cli.zig");
const font_feature = @import("../font/shaper/feature.zig");
const inputpkg = @import("../input.zig");
const Metrics = @import("../font/Metrics.zig");
const shell_integration = @import("../termio/shell_integration.zig");
const terminal_color = @import("../terminal/color.zig");
const conditional = @import("conditional.zig");
const themepkg = @import("theme.zig");
const global = @import("../global.zig");
const String = @import("../main_c.zig").String;

const Config = @import("Config.zig");
const Binding = inputpkg.Binding;

const maximum_frontend_config_size = 1024 * 1024;
const frontend_config_keys = std.StaticStringMap(void).initComptime(.{
    .{"single-instance"},
    .{"tabs-location"},
    .{"wide-tabs"},
    .{"horizontal-tab-scroll"},
    .{"quick-terminal-layer"},
    .{"quick-terminal-namespace"},
    .{"pane-enter-transition-shader"},
    .{"pane-exit-transition-shader"},
    .{"pane-enter-transition-duration"},
    .{"pane-exit-transition-duration"},
    // Ghostty documents this as CLI-only. Loading the mixed override after
    // the CLI must not accidentally make its file spelling effective.
    .{"config-default-files"},
});

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
/// project-private JSON v4 schema. The returned JSON or companion diagnostic
/// allocation follows ghostty_string_s ownership and is released by
/// ghostty_string_free.
export fn ghostty_qt_config_json(
    color_scheme: u8,
    probable_cli: u8,
    frontend_config_path_ptr: ?[*]const u8,
    frontend_config_path_len: usize,
    configuration_arguments: ?[*]const [*:0]const u8,
    configuration_argument_count: usize,
    error_message: *String,
) String {
    error_message.* = .empty;
    const theme: ConfigConditionalTheme = switch (color_scheme) {
        0 => .light,
        1 => .dark,
        else => {
            error_message.* = errorMessage("invalid color scheme") catch .empty;
            return .empty;
        },
    };
    if (probable_cli > 1) {
        error_message.* = errorMessage("invalid probable CLI state") catch .empty;
        return .empty;
    }
    if (frontend_config_path_len > 0 and frontend_config_path_ptr == null) {
        error_message.* = errorMessage("invalid frontend config path") catch .empty;
        return .empty;
    }
    if (configuration_argument_count > 0 and configuration_arguments == null) {
        error_message.* = errorMessage("invalid configuration arguments") catch .empty;
        return .empty;
    }
    const frontend_config_path: ?[]const u8 = if (frontend_config_path_len == 0)
        null
    else
        frontend_config_path_ptr.?[0..frontend_config_path_len];
    return configJson(
        theme,
        probable_cli == 1,
        frontend_config_path,
        configuration_arguments,
        configuration_argument_count,
        error_message,
    ) catch |err| {
        if (error_message.ptr == null)
            error_message.* = errorMessage(@errorName(err)) catch .empty;
        return .empty;
    };
}

const ConfigConditionalTheme = conditional.State.Theme;

/// Apply Ghostty's pinned shell-integration command and environment setup to
/// one frontend-provided launch request. This remains a project-private helper
/// protocol: the Qt process sends and receives owned JSON through the isolated
/// config helper, and no Ghostty-internal allocation crosses that process
/// boundary.
export fn ghostty_qt_shell_integration_json(
    request_ptr: [*]const u8,
    request_len: usize,
) String {
    return shellIntegrationJson(request_ptr[0..request_len]) catch |err| {
        std.log.err("error preparing shell integration as JSON err={}", .{err});
        return .empty;
    };
}

const ShellIntegrationJsonCommand = struct {
    kind: []const u8,
    value: ?[]const u8 = null,
    argv: ?[]const []const u8 = null,
    @"default-shell": bool,
};

const ShellIntegrationJsonEnvironment = struct {
    key: []const u8,
    value: []const u8,
};

const ShellIntegrationJsonFeatures = struct {
    cursor: bool,
    sudo: bool,
    title: bool,
    @"ssh-env": bool,
    @"ssh-terminfo": bool,
    path: bool,
};

const ShellIntegrationJsonRequest = struct {
    version: u8,
    @"resource-dir": ?[]const u8,
    command: ShellIntegrationJsonCommand,
    environment: []const ShellIntegrationJsonEnvironment,
    mode: []const u8,
    features: ShellIntegrationJsonFeatures,
    @"cursor-blink": bool,
};

fn decodeBase64(alloc: std.mem.Allocator, encoded: []const u8) ![]u8 {
    const decoder = std.base64.standard.Decoder;
    const decoded_len = try decoder.calcSizeForSlice(encoded);
    const decoded = try alloc.alloc(u8, decoded_len);
    try decoder.decode(decoded, encoded);
    return decoded;
}

fn decodeBase64Z(alloc: std.mem.Allocator, encoded: []const u8) ![:0]u8 {
    const decoder = std.base64.standard.Decoder;
    const decoded_len = try decoder.calcSizeForSlice(encoded);
    const decoded = try alloc.allocSentinel(u8, decoded_len, 0);
    try decoder.decode(decoded[0..decoded_len], encoded);
    if (std.mem.indexOfScalar(u8, decoded, 0) != null)
        return error.EmbeddedNul;
    return decoded;
}

fn shellIntegrationMode(
    mode: []const u8,
) !?shell_integration.Shell {
    if (std.mem.eql(u8, mode, "detect")) return null;
    inline for (@typeInfo(shell_integration.Shell).@"enum".fields) |field| {
        if (std.mem.eql(u8, mode, field.name))
            return @field(shell_integration.Shell, field.name);
    }
    return error.InvalidShellIntegrationMode;
}

fn shellIntegrationCommand(
    alloc: std.mem.Allocator,
    command: ShellIntegrationJsonCommand,
) !Config.Command {
    if (std.mem.eql(u8, command.kind, "shell")) {
        if (command.argv != null) return error.InvalidShellCommand;
        const encoded = command.value orelse return error.InvalidShellCommand;
        return .{ .shell = try decodeBase64Z(alloc, encoded) };
    }
    if (std.mem.eql(u8, command.kind, "direct")) {
        if (command.value != null) return error.InvalidDirectCommand;
        const encoded = command.argv orelse return error.InvalidDirectCommand;
        if (encoded.len == 0) return error.InvalidDirectCommand;

        const result = try alloc.alloc([:0]const u8, encoded.len);
        for (encoded, 0..) |argument, index|
            result[index] = try decodeBase64Z(alloc, argument);
        return .{ .direct = result };
    }
    return error.InvalidCommandKind;
}

fn writeBase64(
    json: *std.json.Stringify,
    alloc: std.mem.Allocator,
    value: []const u8,
) !void {
    const encoder = std.base64.standard.Encoder;
    const encoded = try alloc.alloc(u8, encoder.calcSize(value.len));
    defer alloc.free(encoded);
    try json.write(encoder.encode(encoded, value));
}

fn writeShellIntegrationCommand(
    json: *std.json.Stringify,
    alloc: std.mem.Allocator,
    command: Config.Command,
    default_shell: bool,
) !void {
    try json.beginObject();
    try json.objectField("kind");
    switch (command) {
        .shell => |value| {
            try json.write("shell");
            try json.objectField("value");
            try writeBase64(json, alloc, value);
        },
        .direct => |arguments| {
            try json.write("direct");
            try json.objectField("argv");
            try json.beginArray();
            for (arguments) |argument|
                try writeBase64(json, alloc, argument);
            try json.endArray();
        },
    }
    try json.objectField("default-shell");
    try json.write(default_shell);
    try json.endObject();
}

const ShellIntegrationEnvironmentEntry = struct {
    key: []const u8,
    value: []const u8,
};

fn shellIntegrationEnvironmentLessThan(
    _: void,
    lhs: ShellIntegrationEnvironmentEntry,
    rhs: ShellIntegrationEnvironmentEntry,
) bool {
    return std.mem.lessThan(u8, lhs.key, rhs.key);
}

fn writeShellIntegrationEnvironment(
    json: *std.json.Stringify,
    alloc: std.mem.Allocator,
    environment: *const std.process.Environ.Map,
) !void {
    var entries: std.ArrayList(ShellIntegrationEnvironmentEntry) = .empty;
    defer entries.deinit(alloc);
    var iterator = environment.iterator();
    while (iterator.next()) |entry| {
        try entries.append(alloc, .{
            .key = entry.key_ptr.*,
            .value = entry.value_ptr.*,
        });
    }
    std.mem.sortUnstable(
        ShellIntegrationEnvironmentEntry,
        entries.items,
        {},
        shellIntegrationEnvironmentLessThan,
    );

    try json.beginArray();
    for (entries.items) |entry| {
        try json.beginObject();
        try json.objectField("key");
        try writeBase64(json, alloc, entry.key);
        try json.objectField("value");
        try writeBase64(json, alloc, entry.value);
        try json.endObject();
    }
    try json.endArray();
}

fn shellIntegrationJson(request_json: []const u8) !String {
    var arena = std.heap.ArenaAllocator.init(global.alloc());
    defer arena.deinit();
    const alloc = arena.allocator();

    const request = try std.json.parseFromSliceLeaky(
        ShellIntegrationJsonRequest,
        alloc,
        request_json,
        .{ .allocate = .alloc_if_needed },
    );
    if (request.version != 1) return error.UnsupportedSchemaVersion;

    var environment = std.process.Environ.Map.init(alloc);
    defer environment.deinit();
    for (request.environment) |entry| {
        const key = try decodeBase64(alloc, entry.key);
        const value = try decodeBase64(alloc, entry.value);
        if (key.len == 0 or std.mem.indexOfAny(u8, key, "=\x00") != null)
            return error.InvalidEnvironmentKey;
        if (std.mem.indexOfScalar(u8, value, 0) != null)
            return error.InvalidEnvironmentValue;
        if (environment.get(key) != null)
            return error.DuplicateEnvironmentKey;
        try environment.put(key, value);
    }

    const original_command =
        try shellIntegrationCommand(alloc, request.command);
    var command = original_command;
    const features: Config.ShellIntegrationFeatures = .{
        .cursor = request.features.cursor,
        .sudo = request.features.sudo,
        .title = request.features.title,
        .@"ssh-env" = request.features.@"ssh-env",
        .@"ssh-terminfo" = request.features.@"ssh-terminfo",
        .path = request.features.path,
    };
    try shell_integration.setupFeatures(
        &environment,
        features,
        request.@"cursor-blink",
    );

    var integrated_shell: ?shell_integration.Shell = null;
    if (!std.mem.eql(u8, request.mode, "none")) {
        const force_shell = try shellIntegrationMode(request.mode);
        if (request.@"resource-dir") |encoded_resource_dir| {
            const resource_dir =
                try decodeBase64(alloc, encoded_resource_dir);
            if (resource_dir.len == 0 or resource_dir[0] != '/' or std.mem.indexOfScalar(u8, resource_dir, 0) != null) {
                return error.InvalidResourceDirectory;
            }
            if (try shell_integration.setup(
                alloc,
                resource_dir,
                original_command,
                &environment,
                force_shell,
            )) |integration| {
                integrated_shell = integration.shell;
                command = integration.command;
            }
        }
    } else if (request.@"resource-dir" != null) {
        // Validate the mode even though no setup is attempted.
        return error.UnexpectedResourceDirectory;
    }

    var output: std.Io.Writer.Allocating = .init(global.alloc());
    errdefer output.deinit();
    var json: std.json.Stringify = .{ .writer = &output.writer };
    try json.beginObject();
    try json.objectField("version");
    try json.write(@as(u8, 1));
    try json.objectField("command");
    try writeShellIntegrationCommand(
        &json,
        alloc,
        command,
        request.command.@"default-shell",
    );
    try json.objectField("environment");
    try writeShellIntegrationEnvironment(&json, alloc, &environment);
    try json.objectField("shell");
    if (integrated_shell) |shell| {
        try json.write(@tagName(shell));
    } else {
        try json.write(null);
    }
    try json.endObject();
    return .fromSlice(try output.toOwnedSlice());
}

fn errorMessage(message: []const u8) !String {
    return .fromSlice(try global.alloc().dupe(u8, message));
}

fn configDiagnostics(config: *const Config) !String {
    var output: std.Io.Writer.Allocating = .init(global.alloc());
    errdefer output.deinit();
    for (config._diagnostics.items(), 0..) |diagnostic, index| {
        if (index > 0) try output.writer.writeByte('\n');
        try output.writer.print("{f}", .{diagnostic});
    }
    return .fromSlice(try output.toOwnedSlice());
}

fn configJson(
    color_scheme: ConfigConditionalTheme,
    probable_cli: bool,
    frontend_config_path: ?[]const u8,
    configuration_arguments: ?[*]const [*:0]const u8,
    configuration_argument_count: usize,
    error_message: *String,
) !String {
    var output: std.Io.Writer.Allocating = .init(global.alloc());
    errdefer output.deinit();

    var json: std.json.Stringify = .{ .writer = &output.writer };
    try json.beginObject();
    try json.objectField("version");
    try json.write(@as(u8, 4));

    {
        var config = try loadSelectedConfig(
            color_scheme,
            probable_cli,
            frontend_config_path,
            configuration_arguments,
            configuration_argument_count,
        );
        defer config.deinit();
        if (!config._diagnostics.empty()) {
            error_message.* = try configDiagnostics(&config);
            return error.InvalidConfig;
        }

        const command_uses_default_shell = commandUsesDefaultShell(&config);

        try json.objectField("values");
        try writeValues(&json, &config, command_uses_default_shell);

        try json.objectField("keybindings");
        try writeKeybinds(&json, &config.keybind);
    }

    // Platform defaults let the frontend distinguish unsupported user
    // bindings from unsupported built-ins. Load them only after releasing the
    // current generation so the short-lived helper does not retain both.
    {
        var defaults = try Config.default(global.alloc());
        defer defaults.deinit();

        try json.objectField("default-keybindings");
        try writeKeybinds(&json, &defaults.keybind);
    }
    try json.endObject();

    return .fromSlice(try output.toOwnedSlice());
}

const MixedConfigIterator = struct {
    line: cli.args.LineIterator,

    pub fn next(self: *@This()) ?[]const u8 {
        while (self.line.next()) |argument| {
            const body = std.mem.trimStart(u8, argument, "-");
            const separator = std.mem.indexOfScalar(u8, body, '=') orelse body.len;
            if (frontend_config_keys.has(body[0..separator])) continue;
            return argument;
        }
        return null;
    }

    pub fn location(
        self: *const @This(),
        alloc: std.mem.Allocator,
    ) std.mem.Allocator.Error!?cli.Location {
        return self.line.location(alloc);
    }
};

fn loadFrontendOverrides(config: *Config, path: []const u8) !bool {
    if (!std.fs.path.isAbsolute(path)) return error.InvalidFrontendConfigPath;

    var file = std.Io.Dir.openFileAbsolute(global.io(), path, .{}) catch |err| switch (err) {
        error.FileNotFound => return false,
        else => return err,
    };
    defer file.close(global.io());

    const stat = try file.stat(global.io());
    if (stat.kind != .file) return error.FrontendConfigNotRegular;
    if (stat.size > maximum_frontend_config_size) return error.FrontendConfigTooLarge;

    var buffer: [2048]u8 = undefined;
    var file_reader = file.reader(global.io(), &buffer);
    const reader = &file_reader.interface;
    const bom: []const u8 = &.{ 0xef, 0xbb, 0xbf };
    if (reader.peek(bom.len)) |prefix| {
        if (std.mem.eql(u8, prefix, bom)) reader.toss(bom.len);
    } else |_| {}

    const recursive_start = config.@"config-file".value.items.len;
    var iter: MixedConfigIterator = .{
        .line = .{ .r = reader, .filepath = path },
    };
    try config.loadIter(global.alloc(), &iter);
    try config.expandPaths(std.fs.path.dirname(path) orelse
        return error.InvalidFrontendConfigPath);
    try config.loadRecursiveFilesFrom(
        global.alloc(),
        recursive_start,
        path,
    );
    return true;
}

fn reapplyConfigurationArguments(
    config: *Config,
    raw_arguments: [*]const [*:0]const u8,
    count: usize,
) !void {
    if (count == 0) return;

    const alloc = config.arenaAlloc();
    const arguments = try alloc.alloc([]const u8, count);
    for (arguments, 0..) |*destination, index| {
        destination.* = std.mem.span(raw_arguments[index]);
    }

    const family_fields = .{
        "font-family",
        "font-family-bold",
        "font-family-italic",
        "font-family-bold-italic",
    };
    inline for (family_fields) |field| @field(config, field).overwrite_next = true;
    defer {
        inline for (family_fields) |field|
            @field(config, field).overwrite_next = false;
    }

    var iter = cli.args.sliceIterator(arguments);
    try config.loadIter(global.alloc(), &iter);

    var buffer: [std.fs.max_path_bytes]u8 = undefined;
    const cwd = try std.Io.Dir.cwd().realPathFile(global.io(), ".", &buffer);
    try config.expandPaths(buffer[0..cwd]);
}

fn loadSelectedConfig(
    color_scheme: ConfigConditionalTheme,
    probable_cli: bool,
    frontend_config_path: ?[]const u8,
    configuration_arguments: ?[*]const [*:0]const u8,
    configuration_argument_count: usize,
) !Config {
    // This is Config.load with the requested conditional state installed
    // before any file is parsed. Selecting afterward is insufficient when
    // the inactive theme contains diagnostics because pinned Ghostty
    // deliberately replays diagnostic steps unconditionally.
    var config = try Config.default(global.alloc());
    errdefer config.deinit();
    config._conditional_state.theme = color_scheme;
    try config.loadDefaultFiles(global.alloc());
    try config.loadCliArgs(global.alloc());
    try config.loadRecursiveFiles(global.alloc());
    const frontend_loaded = if (frontend_config_path) |path|
        try loadFrontendOverrides(&config, path)
    else
        false;
    if (frontend_loaded and configuration_argument_count > 0) {
        try reapplyConfigurationArguments(
            &config,
            configuration_arguments.?,
            configuration_argument_count,
        );
    }

    // The helper initializes Ghostty with argc=1 for a known desktop launch,
    // so the upstream finalizer observes the original launch classification.
    _ = probable_cli;
    try config.finalize();
    return config;
}

/// Finalization replaces a missing ordinary command with the resolved login
/// shell. Replay steps retain every effective config assignment, including the
/// selected theme, so inspect them after finalization to preserve whether that
/// shell was a default or an explicit command. Empty values reset the command;
/// later active assignments retain the same precedence as Config.load.
fn commandUsesDefaultShell(config: *const Config) bool {
    var configured = false;
    steps: for (config._replay_steps.items) |step| {
        const arg: ?[]const u8 = switch (step) {
            .arg => |value| value,
            .conditional_arg => |value| active: {
                for (value.conditions) |condition| {
                    if (!config._conditional_state.match(condition)) {
                        break :active null;
                    }
                }
                break :active value.arg;
            },
            .@"-e" => break :steps,
            .expand, .diagnostic => null,
        };
        const value = arg orelse continue;
        const prefix = "--command";
        if (!std.mem.startsWith(u8, value, prefix)) continue;
        const suffix = value[prefix.len..];
        if (suffix.len == 0 or suffix[0] != '=') continue;
        configured = suffix.len > 1;
    }
    return !configured;
}

/// Match termio.DerivedConfig's palette materialization before the effective
/// appearance crosses the helper boundary. Generation is deliberately skipped
/// for the untouched default palette even when palette-generate is enabled.
fn effectivePalette(config: *const Config) terminal_color.Palette {
    if (config.@"palette-generate" and
        config.palette.mask.findFirstSet() != null)
    {
        return terminal_color.generate256Color(
            config.palette.value,
            config.palette.mask,
            config.background.toTerminalRGB(),
            config.foreground.toTerminalRGB(),
            config.@"palette-harmonious",
        );
    }
    return config.palette.value;
}

fn writeValues(
    json: *std.json.Stringify,
    config: *const Config,
    command_uses_default_shell: bool,
) !void {
    try json.beginObject();

    try json.objectField("term");
    try json.beginArray();
    for (config.term) |byte| try json.write(byte);
    try json.endArray();
    try json.objectField("enquiry-response");
    try json.beginArray();
    for (config.@"enquiry-response") |byte| try json.write(byte);
    try json.endArray();
    try json.objectField("command");
    try writeOptionalCommand(json, config.command, command_uses_default_shell);
    try json.objectField("initial-command");
    try writeOptionalCommand(json, config.@"initial-command", false);
    try json.objectField("input");
    try writeInitialInput(json, &config.input);
    try json.objectField("key-remap");
    try writeKeyRemaps(json, &config.@"key-remap");
    try json.objectField("wait-after-command");
    try json.write(config.@"wait-after-command");
    try json.objectField("abnormal-command-exit-runtime");
    // Unlike Config.Duration fields, this value is already an exact u32
    // millisecond count. Every u32 crosses JSON's binary64 number boundary
    // losslessly, so no unit conversion or decimal-string encoding is needed.
    try json.write(config.@"abnormal-command-exit-runtime");
    try json.objectField("shell-integration");
    try json.write(@tagName(config.@"shell-integration"));
    try json.objectField("shell-integration-features");
    try json.beginObject();
    inline for (.{
        "cursor",
        "sudo",
        "title",
        "ssh-env",
        "ssh-terminfo",
        "path",
    }) |feature| {
        try json.objectField(feature);
        try json.write(@field(config.@"shell-integration-features", feature));
    }
    try json.endObject();
    try json.objectField("env");
    try writeEnvironment(json, &config.env);
    try json.objectField("linux-cgroup");
    try json.write(@tagName(config.@"linux-cgroup"));
    try json.objectField("linux-cgroup-memory-limit");
    try writeOptionalDecimalUint64(json, config.@"linux-cgroup-memory-limit");
    try json.objectField("linux-cgroup-processes-limit");
    try writeOptionalDecimalUint64(json, config.@"linux-cgroup-processes-limit");
    try json.objectField("linux-cgroup-hard-fail");
    try json.write(config.@"linux-cgroup-hard-fail");
    try json.objectField("working-directory");
    try writeWorkingDirectory(json, config.@"working-directory" orelse return error.UnfinalizedConfig);
    try json.objectField("title");
    if (config.title) |title| try json.write(title) else try json.write(null);
    try json.objectField("class");
    if (config.class) |class|
        try writeByteArray(json, class)
    else
        try json.write(null);
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
    try json.objectField("font-feature");
    try writeFontFeatures(json, &config.@"font-feature");
    inline for (.{
        .{ "font-variation", &config.@"font-variation" },
        .{ "font-variation-bold", &config.@"font-variation-bold" },
        .{ "font-variation-italic", &config.@"font-variation-italic" },
        .{ "font-variation-bold-italic", &config.@"font-variation-bold-italic" },
    }) |entry| {
        try json.objectField(entry[0]);
        try writeFontVariations(json, entry[1]);
    }
    try json.objectField("font-codepoint-map");
    try writeFontCodepointMap(json, &config.@"font-codepoint-map");
    try json.objectField("font-synthetic-style");
    try json.beginObject();
    inline for (.{ "bold", "italic", "bold-italic" }) |name| {
        try json.objectField(name);
        try json.write(@field(config.@"font-synthetic-style", name));
    }
    try json.endObject();
    try json.objectField("font-shaping-break");
    try json.beginObject();
    try json.objectField("cursor");
    try json.write(config.@"font-shaping-break".cursor);
    try json.endObject();
    try json.objectField("freetype-load-flags");
    try json.beginObject();
    inline for (.{ "hinting", "force-autohint", "monochrome", "autohint", "light" }) |name| {
        try json.objectField(name);
        try json.write(@field(config.@"freetype-load-flags", name));
    }
    try json.endObject();
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
    try json.objectField("alpha-blending");
    try json.write(@tagName(config.@"alpha-blending"));
    try json.objectField("background-opacity");
    try json.write(config.@"background-opacity");
    try json.objectField("background-opacity-cells");
    try json.write(config.@"background-opacity-cells");
    try json.objectField("background-blur");
    try json.write(config.@"background-blur".cval());
    try json.objectField("background-image");
    try writeOptionalConfigPath(json, config.@"background-image");
    try json.objectField("background-image-opacity");
    try json.write(config.@"background-image-opacity");
    try json.objectField("background-image-position");
    try json.write(@tagName(config.@"background-image-position"));
    try json.objectField("background-image-fit");
    try json.write(@tagName(config.@"background-image-fit"));
    try json.objectField("background-image-repeat");
    try json.write(config.@"background-image-repeat");
    try json.objectField("custom-shader");
    try writeConfigPaths(json, &config.@"custom-shader");
    try json.objectField("custom-shader-animation");
    try json.write(@tagName(config.@"custom-shader-animation"));
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
    try json.objectField("window-theme");
    try json.write(@tagName(config.@"window-theme"));
    try json.objectField("window-title-font-family");
    if (config.@"window-title-font-family") |family|
        try json.write(family)
    else
        try json.write(null);
    try json.objectField("window-titlebar-background");
    try writeOptionalRgb(json, config.@"window-titlebar-background");
    try json.objectField("window-titlebar-foreground");
    try writeOptionalRgb(json, config.@"window-titlebar-foreground");
    try json.objectField("window-subtitle");
    try json.write(@tagName(config.@"window-subtitle"));
    try json.objectField("window-width");
    try json.write(config.@"window-width");
    try json.objectField("window-height");
    try json.write(config.@"window-height");
    inline for (.{
        .{ "window-padding-x", config.@"window-padding-x" },
        .{ "window-padding-y", config.@"window-padding-y" },
    }) |entry| {
        try json.objectField(entry[0]);
        try json.beginArray();
        try json.write(entry[1].top_left);
        try json.write(entry[1].bottom_right);
        try json.endArray();
    }
    try json.objectField("window-padding-balance");
    try json.write(@tagName(config.@"window-padding-balance"));
    try json.objectField("window-padding-color");
    try json.write(@tagName(config.@"window-padding-color"));
    try json.objectField("maximize");
    try json.write(config.maximize);
    try json.objectField("fullscreen");
    try json.write(@tagName(config.fullscreen));
    try json.objectField("palette");
    try json.beginArray();
    for (effectivePalette(config)) |color| try writeRgb(json, color);
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
    try json.objectField("minimum-contrast");
    try json.write(config.@"minimum-contrast");
    try json.objectField("vt-kam-allowed");
    try json.write(config.@"vt-kam-allowed");
    try json.objectField("scrollback-limit-bytes");
    if (config.@"scrollback-limit-bytes".optional()) |value|
        try writeDecimalUint64(json, @intCast(value))
    else
        try json.write(null);
    try json.objectField("scrollback-limit-lines");
    if (config.@"scrollback-limit-lines".optional()) |value|
        try writeDecimalUint64(json, @intCast(value))
    else
        try json.write(null);
    try json.objectField("image-storage-limit");
    try json.write(config.@"image-storage-limit");
    try json.objectField("scrollback-compression");
    try json.write(config.@"scrollback-compression");
    try json.objectField("scrollbar");
    try json.write(@tagName(config.scrollbar));
    try json.objectField("desktop-notifications");
    try json.write(config.@"desktop-notifications");
    try json.objectField("progress-style");
    try json.write(config.@"progress-style");
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
    try json.objectField("clipboard-codepoint-map");
    try writeClipboardCodepointMap(json, &config.@"clipboard-codepoint-map");
    try json.objectField("clipboard-write");
    try json.write(@tagName(config.@"clipboard-write"));
    try json.objectField("clipboard-paste-protection");
    try json.write(config.@"clipboard-paste-protection");
    try json.objectField("clipboard-paste-bracketed-safe");
    try json.write(config.@"clipboard-paste-bracketed-safe");
    try json.objectField("scroll-to-bottom");
    try json.beginObject();
    try json.objectField("keystroke");
    try json.write(config.@"scroll-to-bottom".keystroke);
    try json.objectField("output");
    try json.write(config.@"scroll-to-bottom".output);
    try json.endObject();
    try json.objectField("copy-on-select");
    try json.write(@tagName(config.@"copy-on-select"));
    try json.objectField("selection-clear-on-typing");
    try json.write(config.@"selection-clear-on-typing");
    try json.objectField("selection-clear-on-copy");
    try json.write(config.@"selection-clear-on-copy");
    try json.objectField("selection-word-chars");
    try json.beginArray();
    for (config.@"selection-word-chars".codepoints) |codepoint| {
        try json.write(codepoint);
    }
    try json.endArray();
    try json.objectField("click-repeat-interval");
    try json.write(config.@"click-repeat-interval");
    try json.objectField("right-click-action");
    try json.write(@tagName(config.@"right-click-action"));
    try json.objectField("middle-click-action");
    try json.write(@tagName(config.@"middle-click-action"));
    try json.objectField("mouse-reporting");
    try json.write(config.@"mouse-reporting");
    try json.objectField("mouse-shift-capture");
    try json.write(@tagName(config.@"mouse-shift-capture"));
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
    try json.objectField("config-default-files");
    try json.write(config.@"config-default-files");
    try json.objectField("theme-files");
    try writeThemeFiles(json, config.theme);
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
    try json.objectField("quick-terminal-position");
    try json.write(@tagName(config.@"quick-terminal-position"));
    try json.objectField("quick-terminal-size");
    try writeQuickTerminalSize(json, config.@"quick-terminal-size");
    try json.objectField("quick-terminal-screen");
    try json.write(@tagName(config.@"quick-terminal-screen"));
    try json.objectField("quick-terminal-autohide");
    try json.write(config.@"quick-terminal-autohide");
    try json.objectField("quick-terminal-keyboard-interactivity");
    try json.write(@tagName(config.@"quick-terminal-keyboard-interactivity"));
    try json.objectField("command-palette-entry");
    try writeCommandPalette(json, &config.@"command-palette-entry");
    try json.objectField("app-notifications");
    try json.beginObject();
    inline for (.{ "clipboard-copy", "config-reload" }) |name| {
        try json.objectField(name);
        try json.write(@field(config.@"app-notifications", name));
    }
    try json.endObject();
    try json.endObject();
}

fn writeThemeFiles(
    json: *std.json.Stringify,
    theme: ?Config.Theme,
) !void {
    try json.beginArray();
    const value = theme orelse {
        try json.endArray();
        return;
    };

    var arena = std.heap.ArenaAllocator.init(global.alloc());
    defer arena.deinit();
    const alloc = arena.allocator();
    var paths: std.ArrayList([]const u8) = .empty;
    defer paths.deinit(alloc);

    const names = [_][]const u8{ value.light, value.dark };
    for (names) |name| {
        if (std.fs.path.isAbsolute(name)) {
            try appendThemeFile(json, alloc, &paths, name);
            continue;
        }

        var locations: themepkg.LocationIterator = .{
            .arena_alloc = alloc,
        };
        while (try locations.next()) |location| {
            try appendThemeFile(
                json,
                alloc,
                &paths,
                try std.fs.path.join(alloc, &.{ location.dir, name }),
            );
        }
    }
    try json.endArray();
}

fn appendThemeFile(
    json: *std.json.Stringify,
    alloc: std.mem.Allocator,
    paths: *std.ArrayList([]const u8),
    path: []const u8,
) !void {
    for (paths.items) |existing| {
        if (std.mem.eql(u8, existing, path)) return;
    }
    try paths.append(alloc, path);
    try json.write(path);
}

fn writeDecimalUint64(json: *std.json.Stringify, number: u64) !void {
    // Qt's JSON representation is double-valued. Use canonical decimal text
    // so every u64, including maxInt(u64), crosses the private schema exactly.
    var buffer: [32]u8 = undefined;
    try json.write(try std.fmt.bufPrint(&buffer, "{d}", .{number}));
}

fn writeOptionalDecimalUint64(json: *std.json.Stringify, value: ?u64) !void {
    if (value) |number|
        try writeDecimalUint64(json, number)
    else
        try json.write(null);
}

fn writeOptionalCommand(
    json: *std.json.Stringify,
    value: ?Config.Command,
    default_shell: bool,
) !void {
    const command = value orelse {
        try json.write(null);
        return;
    };

    try json.beginObject();
    try json.objectField("kind");
    switch (command) {
        .shell => |shell| {
            try json.write("shell");
            try json.objectField("value");
            try writeByteArray(json, shell);
        },
        .direct => |arguments| {
            try json.write("direct");
            try json.objectField("argv");
            try json.beginArray();
            for (arguments) |argument| try writeByteArray(json, argument);
            try json.endArray();
        },
    }
    try json.objectField("default-shell");
    try json.write(default_shell and command == .shell);
    try json.endObject();
}

fn writeInitialInput(json: *std.json.Stringify, value: anytype) !void {
    var arena = std.heap.ArenaAllocator.init(global.alloc());
    defer arena.deinit();
    const alloc = arena.allocator();

    try json.beginArray();
    for (value.list.items) |source| {
        const parsed = try source.cloneParsed(alloc);
        try json.beginObject();
        try json.objectField("kind");
        switch (parsed) {
            inline else => |bytes, tag| {
                try json.write(@tagName(tag));
                try json.objectField("value");
                try writeByteArray(json, bytes);
            },
        }
        try json.endObject();
    }
    try json.endArray();
}

fn writeSidedModifier(json: *std.json.Stringify, mods: inputpkg.Mods) !void {
    try json.beginObject();
    inline for (.{ "shift", "ctrl", "alt", "super" }) |name| {
        if (@field(mods, name)) {
            try json.objectField("modifier");
            try json.write(name);
            try json.objectField("side");
            try json.write(@tagName(@field(mods.sides, name)));
            try json.endObject();
            return;
        }
    }
    return error.InvalidFinalizedKeyRemap;
}

fn writeKeyRemaps(json: *std.json.Stringify, value: anytype) !void {
    try json.beginArray();
    var iterator = value.map.iterator();
    while (iterator.next()) |entry| {
        try json.beginObject();
        try json.objectField("from");
        try writeSidedModifier(json, entry.key_ptr.*);
        try json.objectField("to");
        try writeSidedModifier(json, entry.value_ptr.*);
        try json.endObject();
    }
    try json.endArray();
}

fn writeOptionalQuickTerminalSize(
    json: *std.json.Stringify,
    value: ?Config.QuickTerminalSize.Size,
) !void {
    const size = value orelse {
        try json.write(null);
        return;
    };

    try json.beginObject();
    try json.objectField("kind");
    switch (size) {
        .pixels => |pixels| {
            try json.write("pixels");
            try json.objectField("value");
            try json.write(pixels);
        },
        .percentage => |percentage| {
            try json.write("percentage");
            try json.objectField("value-bits");
            try writeDecimalUint64(json, @as(u32, @bitCast(percentage)));
        },
    }
    try json.endObject();
}

fn writeQuickTerminalSize(
    json: *std.json.Stringify,
    value: Config.QuickTerminalSize,
) !void {
    try json.beginObject();
    try json.objectField("primary");
    try writeOptionalQuickTerminalSize(json, value.primary);
    try json.objectField("secondary");
    try writeOptionalQuickTerminalSize(json, value.secondary);
    try json.endObject();
}

fn writeCommandPalette(json: *std.json.Stringify, value: anytype) !void {
    try json.beginArray();
    for (value.value.items) |command| {
        var formatted: std.Io.Writer.Allocating = .init(global.alloc());
        defer formatted.deinit();
        try command.action.format(&formatted.writer);

        try json.beginObject();
        try json.objectField("title");
        try json.write(command.title);
        try json.objectField("description");
        try json.write(command.description);
        try json.objectField("action-key");
        try json.write(@tagName(command.action));
        try json.objectField("action");
        try json.write(formatted.written());
        try json.endObject();
    }
    try json.endArray();
}

fn writeEnvironment(json: *std.json.Stringify, value: anytype) !void {
    try json.beginArray();
    var iterator = value.iterator();
    while (iterator.next()) |entry| {
        try json.beginObject();
        try json.objectField("key");
        try writeByteArray(json, entry.key_ptr.*);
        try json.objectField("value");
        try writeByteArray(json, entry.value_ptr.*);
        try json.endObject();
    }
    try json.endArray();
}

fn writeByteArray(json: *std.json.Stringify, value: []const u8) !void {
    try json.beginArray();
    for (value) |byte| try json.write(byte);
    try json.endArray();
}

fn writeKeybinds(json: *std.json.Stringify, keybinds: *const Config.Keybinds) !void {
    var sequence: std.ArrayList(Binding.Trigger) = .empty;
    defer sequence.deinit(global.alloc());

    try json.beginObject();
    try json.objectField("root");
    try json.beginArray();
    try writeSet(json, global.alloc(), &keybinds.set, &sequence);
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
        try writeSet(json, global.alloc(), table.value_ptr, &sequence);
        try json.endArray();
        try json.endObject();
    }
    try json.endArray();
    try json.endObject();
}

fn writeWorkingDirectory(json: *std.json.Stringify, value: Config.WorkingDirectory) !void {
    switch (value) {
        .home, .inherit => try json.write(@tagName(value)),
        .path => |path| try writeByteArray(json, path),
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

fn openTypeTag(bytes: [4]u8) u32 {
    return (@as(u32, bytes[0]) << 24) |
        (@as(u32, bytes[1]) << 16) |
        (@as(u32, bytes[2]) << 8) |
        @as(u32, bytes[3]);
}

fn writeOpenTypeSetting(
    json: *std.json.Stringify,
    tag: [4]u8,
    value: anytype,
) !void {
    try json.beginObject();
    try json.objectField("tag");
    try json.write(openTypeTag(tag));
    try json.objectField("value");
    try json.write(value);
    try json.endObject();
}

fn writeFontFeatures(
    json: *std.json.Stringify,
    configured: *const Config.RepeatableString,
) !void {
    var features: font_feature.FeatureList = .{};
    defer features.deinit(global.alloc());
    try features.features.appendSlice(global.alloc(), &font_feature.default_features);
    for (configured.list.items) |value|
        try features.appendFromString(global.alloc(), value);

    try json.beginArray();
    for (features.features.items) |feature|
        try writeOpenTypeSetting(json, feature.tag, feature.value);
    try json.endArray();
}

fn writeFontVariations(json: *std.json.Stringify, variations: anytype) !void {
    try json.beginArray();
    for (variations.list.items) |variation| {
        const tag = variation.id.str();
        try json.beginObject();
        try json.objectField("tag");
        try json.write(openTypeTag(tag));
        try json.objectField("value-bits");
        try writeDecimalUint64(json, @as(u64, @bitCast(variation.value)));
        try json.endObject();
    }
    try json.endArray();
}

fn writeFontCodepointMap(
    json: *std.json.Stringify,
    mappings: *const Config.RepeatableCodepointMap,
) !void {
    try json.beginArray();
    const ranges = mappings.map.list.items(.range);
    const descriptors = mappings.map.list.items(.descriptor);
    for (ranges, descriptors) |range, descriptor| {
        try json.beginObject();
        try json.objectField("first");
        try json.write(range[0]);
        try json.objectField("last");
        try json.write(range[1]);
        try json.objectField("family");
        try json.write(descriptor.family orelse "");
        try json.endObject();
    }
    try json.endArray();
}

fn writeClipboardCodepointMap(json: *std.json.Stringify, mappings: anytype) !void {
    try json.beginArray();
    const ranges = mappings.map.list.items(.range);
    const replacements = mappings.map.list.items(.replacement);
    for (ranges, replacements) |range, replacement| {
        try json.beginObject();
        try json.objectField("first");
        try json.write(range[0]);
        try json.objectField("last");
        try json.write(range[1]);
        try json.objectField("replacement");
        try json.beginObject();
        try json.objectField("kind");
        switch (replacement) {
            .codepoint => |codepoint| {
                try json.write("codepoint");
                try json.objectField("value");
                try json.write(codepoint);
            },
            .string => |text| {
                try json.write("text");
                try json.objectField("value");
                try json.write(text);
            },
        }
        try json.endObject();
        try json.endObject();
    }
    try json.endArray();
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
    defer modifiers.deinit(global.alloc());
    inline for (metric_config_descriptors) |descriptor| {
        if (@field(config, descriptor.name)) |modifier| {
            try modifiers.put(global.alloc(), descriptor.key, modifier);
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
    try writeConfigPath(json, value);
}

fn writeConfigPath(json: *std.json.Stringify, value: Config.Path) !void {
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

fn writeConfigPaths(json: *std.json.Stringify, paths: *const Config.RepeatablePath) !void {
    try json.beginArray();
    for (paths.value.items) |path| try writeConfigPath(json, path);
    try json.endArray();
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
