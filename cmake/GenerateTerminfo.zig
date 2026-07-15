const std = @import("std");
const ghostty_terminfo = @import("ghostty_terminfo");

pub fn main() !void {
    var buffer: [1024]u8 = undefined;
    var stdout_writer = std.fs.File.stdout().writerStreaming(&buffer);
    try ghostty_terminfo.ghostty.encode(&stdout_writer.interface);
    try stdout_writer.end();
}
