// Formatted output for BareMetal Zig apps, routed through a plain
// libc write() call -- not std.debug.print(). See ZIG.md's "Known
// gaps" section for why: std.debug.print's stderr-locking/tty-
// detection path (and std.Thread/Mutex/Futex more generally) emit
// inlined raw `syscall` x86 instructions that bypass libc entirely,
// even when linking musl with -lc -- there's no libc symbol there for
// this port's patched musl to intercept, and BareMetal doesn't
// service the raw SYSCALL opcode (confirmed by an actual boot crash:
// Exception 0x06 (UD) on a bare `syscall`). Every call in this file
// only ever reaches std.c.write(), a real, interceptable musl symbol.

const std = @import("std");

/// Formats `fmt`/`args` into a fixed on-stack buffer and writes it to
/// stdout (fd 1). Truncates silently past `buf`'s size rather than
/// growing -- there's no heap allocation here on purpose, so this is
/// safe to call before any allocator is set up.
pub fn print(comptime fmt: []const u8, args: anytype) void {
    var buf: [1024]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch return;
    _ = std.c.write(1, msg.ptr, msg.len);
}

/// Same as `print`, to stderr (fd 2).
pub fn eprint(comptime fmt: []const u8, args: anytype) void {
    var buf: [1024]u8 = undefined;
    const msg = std.fmt.bufPrint(&buf, fmt, args) catch return;
    _ = std.c.write(2, msg.ptr, msg.len);
}
