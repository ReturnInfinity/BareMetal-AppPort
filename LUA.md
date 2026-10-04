# Lua support

BareMetal-AppPort ships a [Lua](https://www.lua.org/) 5.5.1 interpreter,
`lua.app`, built against the same musl/lwext4 port every other language
here uses. It follows the same pattern as `python.app` (see
`PYTHON.md`): the interpreter is built once by `./setup.sh`, and your
program is a plain `.lua` file on the EXT2 disk image that `lua.app`
runs at boot. Lua is a much smaller job than CPython, though: there are
no generated sources, no frozen modules, and no stdlib tree to deploy.
All of Lua, including every standard library, is about 30 portable C
files that compile unmodified against this port's musl.

## Running your own program

From the `BareMetal-App` repo root:

```
./1-build.sh hello.lua
./2-run.sh
```

`1-build.sh` copies the script onto `disk.img` as `/lua/main.lua` (via
`port/lua_port/install-main.sh`, which uses `debugfs -w`, so no root and
no loop mount) and builds the unikernel around the prebuilt `lua.app`.
When you only change the script, you only re-deploy it; nothing is
recompiled.

To do the same by hand from this directory:

```
port/lua_port/install-main.sh /path/to/disk.img hello.lua
```

Run it with no script argument to install `port/lua_port/main_test.lua`,
a smoke test that checks the core language and every standard library
against this port's real backends.

### Arguments

The VM's own arguments (`./2-run.sh a b`, passed through the Firecracker
cmdline's `args=` token, see `port/crt0.c`) reach the script the same
way stock `lua script.lua a b` passes them: in the global `arg` table
(`arg[0]` is `/lua/main.lua`) and as the main chunk's `...`.

### Modules

`require()` searches `/lua/?.lua` and `/lua/?/init.lua`
(`LUAMAIN_PACKAGE_PATH` in `port/lua_port/lua.c`), so modules can sit
next to `main.lua`. `install-main.sh` only writes `main.lua`; copy any
other modules into `/lua/` with `debugfs -w -R "write mymod.lua
/lua/mymod.lua" disk.img`.

## How it's built

- `scripts/get-lua.sh` fetches the pinned 5.5.1 tarball from lua.org and
  checks its published SHA-256.
- `setup.sh` compiles every `src/*.c` except `lua.c` (the standalone
  `lua` command) and `luac.c` (the bytecode compiler) into
  `build/lua_*.o`. That is the same file set as Lua's own `CORE_O` plus
  `LIB_O`, built with the same freestanding flags as everything else in
  `setup.sh` plus `-DLUA_USE_POSIX`.
- `build-app.sh` links `build/lua_*.o` into every app, the same way
  curl and SQLite are linked. `--gc-sections` drops it from apps that
  don't use it, and any C app can embed Lua by including `lua.h` (the
  `-I` path is already set).
- `setup.sh` then builds `lua.app` from this port's own entry point,
  `port/lua_port/lua.c`. To rebuild it after changing that file:
  `./build-app.sh port/lua_port/lua.c`

### `LUA_USE_POSIX`, not `LUA_USE_LINUX`

`LUA_USE_LINUX` is `LUA_USE_POSIX` plus `LUA_USE_DLOPEN`. The POSIX half
is fully backed here:

- `_setjmp`/`_longjmp` for error handling
- `mkstemp()` for `os.tmpname()`
- `fseeko`/`ftello` for `file:seek()`
- `popen()` for `io.popen()`. This fails cleanly: musl's `popen()` and
  `system()` both stop at `pipe2()`, which returns `-ENOSYS` on this
  port, so `io.popen()` and `os.execute()` just return a Lua error.

The `dlopen` half is left off. `port/dlfcn_shim.c` can load a module,
but its curated `dl_exports[]` table doesn't expose the Lua C API that a
binary module would need to call back into.

### `port/lua_port/lua.c`

This replaces stock `src/lua.c`. There's no shell to type `lua -e ...`
into and no terminal for a REPL, so instead it:

- opens a state with `luaL_openlibs()`
- sets `package.path` as described above and empties `package.cpath`
- builds `arg`
- runs `/lua/main.lua` under `lua_pcall()`, with a traceback message
  handler like stock `lua.c`, so an uncaught error prints where it
  happened

It exits 0 on success and 1 on error.

Like `python.c`, it switches to its own C stack before doing any work,
because BareMetal's kernel stack is a fixed 64 KiB. Lua recurses on the
C stack for Lua->C->Lua chains (`pcall`, `string.gsub` callbacks,
metamethods, coroutines) and in its parser, up to `LUAI_MAXCCALLS`
(200) levels. The worst case measured was a `string.gsub` callback
chain: about 402 KiB at the point where Lua's own "C stack overflow"
check stopped it, because each level keeps a 1 KiB `luaL_Buffer` on the
C stack. The stack is therefore 512 KiB. 256 KiB was tried first and
silently overran into neighbouring `.bss`. The stack can't be much
larger either: it lives in `.bss`, inside the same 2 MiB boot RAM window
the heap starts in.

## What's supported

All of the standard library: `base`, `package` (Lua modules only),
`coroutine`, `string`, `utf8`, `table`, `math`, `io`, `os` and `debug`.

| Area | Backed by |
|---|---|
| File I/O, `os.remove`, `os.rename` | `ext4_shim.c` on the real EXT2 disk |
| `os.time`, `os.clock`, `os.date` | `posix_shim.c`'s clocks |
| `print`, `io.write` | the serial console |

`port/lua_port/main_test.lua` checks each of these on every run.

## What's not supported

- **No processes.** `io.popen()` and `os.execute()` return errors,
  matching `OPENISSUES.md`'s Process model section.
- **No binary (C) modules.** `package.cpath` is empty; see
  `LUA_USE_POSIX` above. To add a C library, link it into `lua.app` and
  register it with `luaL_requiref()` in `lua.c`.
- **No sockets.** Stock Lua has no networking library at all.
  LuaSocket, or a small built-in module over `port/net_shim.c`, would be
  the next step.
- **No interactive REPL.** There's no terminal on the serial console to
  drive one.
- **The heap is small without hot-plug.** `lua.app` starts with roughly
  1 MiB of heap in the 2 MiB boot window. Past that, it relies on
  virtio-mem hot-plug, exactly like every other app (see
  `OPENISSUES.md`'s Heap section).
