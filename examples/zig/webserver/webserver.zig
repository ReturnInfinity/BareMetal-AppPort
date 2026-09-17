// webserver.zig -- Zig counterpart to webserver.c/webserver-rs/
// webserver.py: a single-threaded HTTP server with a hit counter, built
// on std.net.Address.listen/Server.accept. Exercises the musl ->
// posix_shim -> net_shim -> lwIP TCP server-side (bind/listen/accept)
// path via real std, the same way the other languages' webservers
// already do -- and, since Zig's socket calls route through libc the
// same way its file I/O does (see ZIG.md), this needs nothing from the
// SYSCALL/SYSRET fix std.debug.print/std.Thread depend on.

const std = @import("std");

const port: u16 = 80;

var hit_count: u64 = 0;

// std.net.Stream.write()/writeAll() (the current, non-deprecated API,
// backed by Zig 0.15's Io.Writer machinery) sends via a real sendmsg()
// syscall -- this port's posix_shim.c has no SYS_sendmsg case at all
// (net_shim_send() is only reachable from plain write()/SYS_write, which
// sys_write() already special-cases for socket fds). That surfaced here
// as every response silently failing with error.Unexpected (posix_shim's
// -ENOSYS default case, mapped by Zig's error set to something it
// doesn't expect from a real Linux write()) -- curl saw a connection
// that read the request fine and then closed with no response at all.
// std.posix.write() below is the *old*, plain SYS_write wrapper
// (Stream.write() used this too before Zig 0.15's Io.Writer rework) and
// works correctly through the exact same sys_write()/net_shim_send()
// path stream.read() already uses on the way in. See ZIG.md's "Known
// gaps" for the SYS_sendmsg/SYS_recvmsg gap this works around.
fn writeAll(stream: std.net.Stream, bytes: []const u8) std.posix.WriteError!void {
    var index: usize = 0;
    while (index < bytes.len)
        index += try std.posix.write(stream.handle, bytes[index..]);
}

fn handleConnection(stream: std.net.Stream, peer: std.net.Address) void {
    defer stream.close();

    var buf: [4096]u8 = undefined;
    const n = stream.read(&buf) catch return;

    // Don't bother parsing the request -- just grab the first line for
    // the log message, matching webserver-rs's posture.
    var request_line: []const u8 = "";
    if (std.mem.indexOfScalar(u8, buf[0..n], '\n')) |eol| {
        request_line = std.mem.trimRight(u8, buf[0..eol], "\r");
    }

    hit_count += 1;

    var log_buf: [256]u8 = undefined;
    if (std.fmt.bufPrint(&log_buf, "Connection from {f} - {s}\n", .{ peer, request_line })) |msg| {
        _ = std.c.write(1, msg.ptr, msg.len);
    } else |_| {}

    var body_buf: [2048]u8 = undefined;
    const body = std.fmt.bufPrint(&body_buf,
        \\<!DOCTYPE html>
        \\<html lang="en">
        \\<head>
        \\<meta charset="utf-8">
        \\<title>BareMetal-App</title>
        \\<style>
        \\  body {{
        \\    margin: 0;
        \\    padding: 3rem 1.5rem;
        \\    background: #0f172a;
        \\    color: #e2e8f0;
        \\    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
        \\    display: flex;
        \\    justify-content: center;
        \\  }}
        \\  .card {{
        \\    background: #1e293b;
        \\    border-radius: 12px;
        \\    padding: 2rem 2.5rem;
        \\    max-width: 640px;
        \\    width: 100%;
        \\    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.3);
        \\  }}
        \\  h1 {{
        \\    margin-top: 0;
        \\    font-size: 1.5rem;
        \\    color: #f7a41d;
        \\  }}
        \\  .hits {{
        \\    font-size: 2.5rem;
        \\    font-weight: 700;
        \\    color: #f8fafc;
        \\    margin: 0.25rem 0 1.5rem;
        \\  }}
        \\  table {{
        \\    width: 100%;
        \\    border-collapse: collapse;
        \\    font-size: 0.9rem;
        \\  }}
        \\  th, td {{
        \\    text-align: left;
        \\    padding: 0.5rem 0;
        \\    border-bottom: 1px solid #334155;
        \\  }}
        \\  th {{
        \\    color: #94a3b8;
        \\    font-weight: 500;
        \\    width: 40%;
        \\  }}
        \\  td {{
        \\    color: #e2e8f0;
        \\    word-break: break-all;
        \\  }}
        \\</style>
        \\</head>
        \\<body>
        \\  <div class="card">
        \\    <h1>Hello!</h1>
        \\    <p class="hits">{d} hit(s)</p>
        \\    <table>
        \\      <tr><th>Zig version</th><td>{s}</td></tr>
        \\      <tr><th>Target triple</th><td>x86_64-linux-musl (BareMetal-AppPort)</td></tr>
        \\      <tr><th>Platform</th><td>BareMetal unikernel</td></tr>
        \\    </table>
        \\  </div>
        \\</body>
        \\</html>
        \\
    , .{ hit_count, @import("builtin").zig_version_string }) catch return;

    var head_buf: [256]u8 = undefined;
    const head = std.fmt.bufPrint(&head_buf, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {d}\r\nConnection: close\r\n\r\n", .{body.len}) catch return;

    writeAll(stream, head) catch return;
    writeAll(stream, body) catch return;
}

export fn main(argc: c_int, argv: [*c][*c]u8, envp: [*c][*c]u8) callconv(.c) c_int {
    _ = argc;
    _ = argv;
    _ = envp;

    const address = std.net.Address.parseIp4("0.0.0.0", port) catch {
        std.debug.print("parseIp4 failed\n", .{});
        return 1;
    };

    var server = address.listen(.{ .reuse_address = true }) catch {
        std.debug.print("listen() on port {d} failed\n", .{port});
        return 1;
    };
    defer server.deinit();

    std.debug.print("listening on port {d}\n", .{port});

    while (true) {
        const conn = server.accept() catch continue;
        handleConnection(conn.stream, conn.address);
    }
}
