const std = @import("std");

export fn main(argc: c_int, argv: [*c][*c]u8, envp: [*c][*c]u8) callconv(.c) c_int {
    _ = argc;
    _ = argv;
    _ = envp;
    std.debug.print("Hello from Zig!\n", .{});
    return 0;
}
