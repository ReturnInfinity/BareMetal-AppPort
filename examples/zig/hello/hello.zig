const bm = @import("bm");

export fn main(argc: c_int, argv: [*c][*c]u8, envp: [*c][*c]u8) callconv(.c) c_int {
    _ = argc;
    _ = argv;
    _ = envp;
    bm.print("Hello from Zig!\n", .{});
    return 0;
}
