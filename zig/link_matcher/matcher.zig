const std = @import("std");
const oni = @import("oniguruma");
const ghostty_url = @import("ghostty_url");

const allocator = std.heap.c_allocator;
const oni_search_retry_limit = 100_000;

const Status = enum(c_int) {
    match = 0,
    no_match = 1,
    invalid_argument = 2,
    engine_error = 3,
};

const Match = extern struct {
    begin: usize,
    end: usize,
};

const Matcher = struct {
    regex: oni.Regex,
};

var initialization_mutex: std.Thread.Mutex = .{};
var oniguruma_initialized = false;

fn ensureOnigurumaInitialized() !void {
    initialization_mutex.lock();
    defer initialization_mutex.unlock();

    if (oniguruma_initialized) return;
    try oni.init(&.{oni.Encoding.utf8});
    oniguruma_initialized = true;
}

export fn ghostty_qt_link_matcher_create() ?*Matcher {
    ensureOnigurumaInitialized() catch return null;

    const matcher = allocator.create(Matcher) catch return null;
    matcher.* = .{
        .regex = oni.Regex.init(
            ghostty_url.regex,
            .{},
            oni.Encoding.utf8,
            oni.Syntax.default,
            null,
        ) catch {
            allocator.destroy(matcher);
            return null;
        },
    };
    return matcher;
}

export fn ghostty_qt_link_matcher_destroy(matcher: ?*Matcher) void {
    const value = matcher orelse return;
    value.regex.deinit();
    allocator.destroy(value);
}

export fn ghostty_qt_link_matcher_find_next(
    matcher: ?*Matcher,
    input_pointer: ?[*]const u8,
    input_length: usize,
    search_offset: usize,
    out_match: ?*Match,
) c_int {
    const value = matcher orelse return @intFromEnum(Status.invalid_argument);
    const output = out_match orelse return @intFromEnum(Status.invalid_argument);
    if (search_offset > input_length or
        (input_length > 0 and input_pointer == null))
    {
        return @intFromEnum(Status.invalid_argument);
    }

    output.* = .{ .begin = 0, .end = 0 };
    if (search_offset == input_length) return @intFromEnum(Status.no_match);

    const input = input_pointer.?[0..input_length];
    var match_parameter = oni.MatchParam.init() catch
        return @intFromEnum(Status.engine_error);
    defer match_parameter.deinit();
    match_parameter.setRetryLimitInSearch(oni_search_retry_limit) catch
        return @intFromEnum(Status.engine_error);

    var region = value.regex.searchWithParam(
        input[search_offset..],
        .{},
        &match_parameter,
    ) catch |err| switch (err) {
        error.Mismatch => return @intFromEnum(Status.no_match),
        else => return @intFromEnum(Status.engine_error),
    };
    defer region.deinit();

    const starts = region.starts();
    const ends = region.ends();
    if (starts.len == 0 or ends.len == 0 or starts[0] < 0 or ends[0] < 0) {
        return @intFromEnum(Status.engine_error);
    }

    output.* = .{
        .begin = search_offset + @as(usize, @intCast(starts[0])),
        .end = search_offset + @as(usize, @intCast(ends[0])),
    };
    if (output.end <= output.begin or output.end > input_length) {
        output.* = .{ .begin = 0, .end = 0 };
        return @intFromEnum(Status.engine_error);
    }
    return @intFromEnum(Status.match);
}

test "C boundary finds successive matches at byte offsets" {
    const matcher = ghostty_qt_link_matcher_create() orelse
        return error.TestUnexpectedResult;
    defer ghostty_qt_link_matcher_destroy(matcher);

    const input = "see https://example.com then src/config/url.zig";
    var found: Match = undefined;
    try std.testing.expectEqual(
        @intFromEnum(Status.match),
        ghostty_qt_link_matcher_find_next(
            matcher,
            input.ptr,
            input.len,
            0,
            &found,
        ),
    );
    try std.testing.expectEqualStrings(
        "https://example.com",
        input[found.begin..found.end],
    );

    try std.testing.expectEqual(
        @intFromEnum(Status.match),
        ghostty_qt_link_matcher_find_next(
            matcher,
            input.ptr,
            input.len,
            found.end,
            &found,
        ),
    );
    try std.testing.expectEqualStrings(
        "src/config/url.zig",
        input[found.begin..found.end],
    );
}

test "C boundary rejects invalid arguments without dereferencing them" {
    var found: Match = undefined;
    try std.testing.expectEqual(
        @intFromEnum(Status.invalid_argument),
        ghostty_qt_link_matcher_find_next(null, null, 0, 0, &found),
    );

    const matcher = ghostty_qt_link_matcher_create() orelse
        return error.TestUnexpectedResult;
    defer ghostty_qt_link_matcher_destroy(matcher);
    try std.testing.expectEqual(
        @intFromEnum(Status.invalid_argument),
        ghostty_qt_link_matcher_find_next(matcher, null, 1, 0, &found),
    );
}
