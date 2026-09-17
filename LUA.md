# Lua support

BareMetal-AppPort embeds the Lua 5.4 interpreter against the same
musl/lwIP/mbedTLS/lwext4 port the C/C++/Python/Rust sides already use.
See `build-app.sh`'s own header for the C side and `PYTHON.md`/
`CPP.md`/`RUST.md` for the Python/C++/Rust equivalents; this document
is the Lua one.

## Why this was the easy one

Lua's own `src/*.c` is portable ANSI C with every OS-specific piece
gated behind a `LUA_USE_*` preprocessor macro that upstream Lua itself
defines (`LUA_USE_LINUX`, `LUA_USE_POSIX`, `LUA_USE_DLOPEN`, ...) --
`loadlib.c`'s `dlopen`-based dynamic library loading, `loslib.c`'s
`mkstemp`-based `os.tmpname` and `<unistd.h>` include,  `liolib.c`'s
`fseeko`/`ftello` (`l_fseek`), and `lauxlib.c`'s `<sys/wait.h>`-based
`pclose` status macros are all `#if defined(LUA_USE_POSIX)`, never
unconditional. Simply defining **none** of them (the same "plain ISO
C" fallback countless other platforms already build Lua with) compiles
every one of these files as-is against musl, with no shim needed at
all beyond `port/lua_port/lua.c` (this port's own replacement for
upstream's `src/lua.c`, playing the same role `python.c` plays for
CPython -- see below). Confirmed by compiling each of Lua's `src/*.c`
files standalone against this port's musl headers before wiring
anything else in.

This is a deliberate scope choice, not an oversight: it means
`package.loadlib`/`require`-ing a compiled C module doesn't work here
(no dynamic loading), `os.tmpname()` falls back to ISO C `tmpnam()`
instead of `mkstemp()`, and `io.popen` doesn't exist -- all consistent
with this port's "no real process model" posture already documented in
`OPENISSUES.md` and taken by Python's own `install_signal_handlers = 0`
(see `python.c`). `os.execute()` still compiles (`system()` is ISO C,
unconditional in `loslib.c`) but has nothing under it to actually fork/
exec a process; it fails at runtime rather than at link time.

## `lua.c`: this port's own entry point

Upstream Lua's own `src/lua.c` parses a real `argv`, offers an
interactive REPL over stdin when given no script, and installs a
`SIGINT` handler via `<signal.h>`. None of that fits this port the same
way it doesn't fit Python's embedding (see `python.c`'s own header
comment): `crt0.c` fabricates an empty `argv`/`envp`, there's no real
terminal on the other end of this port's serial console input path to
be interactive with, and there's no real signal delivery to install a
handler for.

`port/lua_port/lua.c` replaces it: `luaL_newstate()` +
`luaL_openlibs()` + `luaL_loadfile()`/`lua_pcall()` on
`LUAMAIN_SCRIPT_PATH` (`/lualib/main.lua`, a real file on the EXT2 disk
image -- see `install-main.sh`), reporting any error to stderr the same
way `lua.c`'s own `l_message`/`report` do. It's built in place of
upstream's `src/lua.c` (excluded from `setup.sh`'s `LUA_SRCS` list,
along with `luac.c`, the standalone bytecode compiler -- neither is
built here), not a patch on top of it.

## Build flow

Same shape as `python.app`: `setup.sh` compiles every one of Lua's
`src/*.c` files (minus `lua.c`/`luac.c`) once into `build/luacore_*.o`
using the same freestanding CFLAGS every other library here uses
(`-ffreestanding -nostdlib -fno-pic -fno-pie -mcmodel=large
-mno-red-zone -nostdinc -isystem $MUSL_INC`, no extra defines), then
calls `build-app.sh port/lua_port/lua.c` to link `lua.app`. Like
`python_*.o`, `build-app.sh`/`build-cpp-app.sh`/`build-rust-app.sh` all
link `luacore_*.o` into *every* app they build, whatever language it's
written in -- `--gc-sections` drops what a given app doesn't reach.

(Named `luacore_*.o`, not `lua_*.o`: build-app.sh names an app's own
compiled object after its source file's basename with no prefix, e.g.
`port/lua_port/lua.c` -> `build/lua.o` -- a `lua_*.o` glob would have
collided with a hypothetical app source file starting with `lua_`, the
same double-linking failure mode `sqlite_vfs.o` avoids by not sharing
`sqlite_*.o`'s glob either. `lua.c` itself doesn't collide since it
doesn't match `luacore_*`, but this was actually hit and fixed during
development -- see git history.)

`build-app.sh`'s `APP_CFLAGS`/`build-cpp-app.sh`'s equivalent also gain
`-I $LUA_DIR/src`, so any app (not just `lua.c` itself) can
`#include "lua.h"`/`"lauxlib.h"`/`"lualib.h"` directly and embed the
interpreter, the same way `test-mbedtls.c`/`sqltest.c` reach mbedTLS/
SQLite's own APIs directly instead of only through a shim.

## Running your own program

The top-level wrapper's `./1-build.sh yourscript.lua` deploys
`yourscript.lua` onto `disk.img` as `/lualib/main.lua` via
`install-main.sh` (debugfs -w, no host root/loop-mount needed, safe
even while a VM has the image open -- same approach as
`python_port/install-main.sh`), then builds the unikernel around the
already-built `lua.app` instead of compiling anything fresh. Unlike
Python, there's no separate stdlib deploy step -- Lua's standard
library is compiled straight into the interpreter itself
(`setup.sh`'s `LUA_SRCS`), so only the script itself needs to land on
disk.

To embed Lua directly in a C/C++ app instead of using the standalone
interpreter, `#include "lua.h"` etc. (resolved via the `-I` above) and
link normally through `build-app.sh`/`build-cpp-app.sh` -- no extra
step needed, `luacore_*.o` is already linked in.

## Verified so far

Booted for real under Firecracker (`port/lua_port/main_test.lua`,
deployed via `install-main.sh`, run through the full
`./1-build.sh`/`baremetal.sh start` pipeline): core language
(closures, metatables/`__tostring`), the `string`/`table`/`math`
libraries, coroutines (`coroutine.create`/`resume`/`yield`/`status`),
real file I/O against the EXT2 disk image (`io.open`/`write`/`read`/
`os.remove`, going through `ext4_shim.c` the same way every other
app's file I/O does), and `pcall`-based error handling. `examples/lua/
hello/hello.lua` (`print("Hello from Lua!")`) also boots and prints
correctly.

## Known gaps

- No dynamic C module loading (`package.loadlib`/`require` for a
  compiled `.so`-equivalent) -- `loadlib.c`'s POSIX/dlopen backend is
  never enabled (see "Why this was the easy one" above); pure-Lua
  `require` targets would still need a real `package.path` search over
  files on `disk.img`, which nothing here sets up yet (only the fixed
  `/lualib/main.lua` entry point is wired in).
- `os.execute()`/`io.popen` compile but have no process model under
  them to actually do anything (consistent with `OPENISSUES.md`).
- No dedicated larger C stack the way `python.c` switches to for
  CPython's own recursion guard -- Lua's C recursion depth is far
  shallower than CPython's, and crt0.c's normal per-app stack was
  sufficient for everything tested so far. A very deeply recursive
  script could still in principle exhaust it; if that's ever hit in
  practice, `python.c`'s own `python_c_stack`/inline-asm `leaq`+`call`
  pattern is the template to copy.
- Locale is always "C"/POSIX (no `setlocale()` target other than that
  exists here), same posture as every other language in this port.

## Toolchain

Pinned to Lua 5.4.7, vendored unmodified via `scripts/get-lua.sh`
(the official `lua.org` source tarball, not a git checkout) into
`build/lua-5.4.7/` -- all port-side work lives in `port/lua_port/`
instead of patches to Lua's own source, the same choice already made
for lwIP/Mbed TLS/curl/SQLite/lwext4/libsodium/CPython.
