// Formatted output for BareMetal Zig apps, routed through a plain libc
// write() call rather than std.debug.print(). Originally written as a
// mandatory workaround: std.debug.print's stderr-locking/tty-detection
// path (and std.Thread/Mutex/Futex more generally) emits inlined raw
// `syscall` x86 instructions that bypass libc entirely, and this port had
// no way to service that raw opcode at all (confirmed by an actual boot
// crash: Exception 0x06 (UD) on a bare `syscall`).
//
// That's no longer true -- BareMetal-Firecracker gained a real
// SYSCALL/SYSRET kernel path (see its interrupt.asm's int_syscall_fast
// and ZIG.md's "SYSCALL/SYSRET" section) specifically to service those
// raw instructions too, and std.debug.print/std.Thread are now verified
// working end to end (real boot, both plain and formatted prints, and a
// real std.Thread.spawn/join + atomics test). This file is kept as an
// optional, lighter-weight alternative -- no locking, no heap, no
// dependency on the Firecracker kernel fix at all (routes through libc
// the same way every other language here already does) -- not because
// std.debug.print is unsafe anymore.

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
