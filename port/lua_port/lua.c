// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// lua.c -- this port's own "main()" for the Lua interpreter, in place of
// Lua's stock src/lua.c (the standalone `lua` command: option parsing,
// LUA_INIT, an interactive REPL on a terminal). None of that fits here
// -- there's no shell to type `lua -e ...` into and no real terminal on
// the other end of the serial console -- so this is the same shape as
// port/python_port/python.c instead: open a fresh state with the full
// standard library, then run LUAMAIN_SCRIPT_PATH -- a real .lua file on
// the EXT2 disk image (see port/lua_port/install-main.sh) -- as the
// program. Replace that file's content to run something else; nothing
// here needs to change. See LUA.md for the full account.
//
// Unlike python.c, the whole interpreter is ordinary embedding-API
// usage (luaL_newstate()/luaL_openlibs()/luaL_loadfile()/lua_pcall()):
// Lua needs no generated sources, no frozen modules, and no stdlib
// tree on disk -- every standard library is plain C, linked in from
// setup.sh's build/lua_*.o.
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// The program to run -- a real file on the EXT2 disk image, not baked
// into this binary. port/lua_port/install-main.sh writes it there.
#define LUAMAIN_SCRIPT_PATH "/lua/main.lua"

// Where require() looks, in place of luaconf.h's LUA_PATH_DEFAULT
// (/usr/local/share/lua/5.5/..., an installed-prefix layout this port
// doesn't have): next to main.lua itself, so a program can ship its own
// modules alongside it (install-main.sh only writes main.lua; copy
// others into /lua/ the same way). No package.cpath -- see LUA.md's
// "What's not supported" on C modules.
#define LUAMAIN_PACKAGE_PATH "/lua/?.lua;/lua/?/init.lua"

// Lua recurses on the C stack for Lua->C->Lua call chains (pcall,
// string.gsub/table.sort callbacks, metamethods, coroutine resumes)
// and in its recursive-descent parser, bounded at LUAI_MAXCCALLS (200)
// levels -- and BareMetal's own kernel/monitor stack is a fixed 64 KiB
// (see python.c's matching comment). So lua.app switches to its own
// stack before doing any real work, exactly like python.c: exit()
// (posix_shim.c's sys_exit()) restores RSP from the original entry
// point directly, so nothing ever needs to unwind back through main()'s
// abandoned frame. `call`, not `jmp`, keeps RSP at the SysV ABI's
// 8-mod-16 offset for lua_main()'s prologue. lua_main() is non-static
// and the stack is __attribute__((used)) because the asm below
// references both by name, invisible to the compiler's own use
// analysis (see python.c).
//
// 512 KiB, sized from measurement rather than python.c's 1 MiB: the
// worst case found is a string.gsub() callback chain (each level keeps
// a LUAL_BUFFERSIZE luaL_Buffer on the C stack), which peaked at ~402
// KiB (a painted-stack high-water mark) when Lua's own "C stack
// overflow" check stopped it at LUAI_MAXCCALLS; deep metamethod/pcall
// chains and nested-expression parsing all stay well under that. 256
// KiB was tried first and silently overran into neighbouring .bss
// (musl's malloc state) on that same gsub chain. Not bigger, because
// this array lives in .bss -- carved out of the same boot RAM window
// the heap starts in (2 MiB before any hot-plug, see OPENISSUES.md's
// Heap section) -- so every KiB here is one Lua's own allocations
// can't use on a VM with no virtio-mem budget.
#define LUA_C_STACK_BYTES (512 * 1024)
__attribute__((used)) static unsigned char lua_c_stack[LUA_C_STACK_BYTES] __attribute__((aligned(16)));

// main()'s own arguments live in registers/its own frame, both gone
// once RSP moves -- parked here for lua_main() to pick up instead.
static int g_argc;
static char **g_argv;

void lua_main(void) __attribute__((noreturn));

int main(int argc, char **argv)
{
	g_argc = argc;
	g_argv = argv;
	__asm__ volatile(
		"leaq lua_c_stack+%c0(%%rip), %%rsp\n\t"
		"call lua_main\n\t"
		:: "i" (LUA_C_STACK_BYTES) : "memory"
	);
	__builtin_unreachable();
}

// Error handler for the main chunk's lua_pcall(): appends a traceback,
// same as stock lua.c's msghandler(), so an uncaught error prints where
// it happened rather than just its message.
static int msghandler(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);
	if (msg == NULL) {
		if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
			return 1;
		msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
	}
	luaL_traceback(L, L, msg, 1);
	return 1;
}

void lua_main(void)
{
	int rc = 1;

	lua_State *L = luaL_newstate();
	if (L == NULL) {
		fprintf(stderr, "lua.c: cannot create state: not enough memory\n");
		exit(1);
	}
	luaL_openlibs(L);

	lua_getglobal(L, "package");
	lua_pushstring(L, LUAMAIN_PACKAGE_PATH);
	lua_setfield(L, -2, "path");
	lua_pushstring(L, "");
	lua_setfield(L, -2, "cpath");
	lua_pop(L, 1);

	// Global `arg`, same layout stock lua.c builds: arg[0] is the
	// script, arg[1..] the VM's own args (crt0.c's argv[1..], from the
	// Firecracker cmdline's args= token). argv[0] is crt0.c's fixed
	// "main" placeholder, not a real program name, so the script path
	// stands in for it.
	lua_createtable(L, g_argc > 1 ? g_argc - 1 : 0, 1);
	lua_pushstring(L, LUAMAIN_SCRIPT_PATH);
	lua_rawseti(L, -2, 0);
	for (int i = 1; i < g_argc; i++) {
		lua_pushstring(L, g_argv[i]);
		lua_rawseti(L, -2, i);
	}
	lua_setglobal(L, "arg");

	lua_pushcfunction(L, msghandler);
	int base = lua_gettop(L);
	if (luaL_loadfile(L, LUAMAIN_SCRIPT_PATH) != LUA_OK) {
		fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
	} else {
		// The same args again as the main chunk's `...`, as stock lua.c
		// passes them.
		for (int i = 1; i < g_argc; i++)
			lua_pushstring(L, g_argv[i]);
		int nargs = g_argc > 1 ? g_argc - 1 : 0;
		if (lua_pcall(L, nargs, 0, base) != LUA_OK)
			fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
		else
			rc = 0;
	}

	lua_close(L);
	// See LUA_C_STACK_BYTES's comment: there's no valid frame to
	// `return` into, so exit() directly.
	exit(rc);
}
