const std = @import("std");
const ghostty_terminfo = @import("ghostty_terminfo");

pub fn main(init: std.process.Init) !void {
    var buffer: [1024]u8 = undefined;
    var stdout_writer = std.Io.File.stdout().writerStreaming(init.io, &buffer);
    try ghostty_terminfo.ghostty.encode(&stdout_writer.interface);
    try stdout_writer.end();
}
