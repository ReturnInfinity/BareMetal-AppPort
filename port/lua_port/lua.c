// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// lua.c -- this port's own "main()" for the Lua interpreter, in place
// of upstream Lua's own src/lua.c of the same name (which parses a
// real argv, offers an interactive REPL over stdin, and installs a
// SIGINT handler via <signal.h>). Not built from the vendored
// build/lua-5.4.7/src/lua.c at all -- see setup.sh's LUA_SRCS list,
// which deliberately excludes it (and luac.c). None of that upstream
// behavior fits this port: crt0.c fabricates an empty argv/envp
// (there's no real command line to parse), there's no real terminal on
// the other end of this port's serial console input path to be
// interactive with, and there's no real signal delivery to install a
// SIGINT handler for (see port/python_port/python.c's own comment on
// the same point).
//
// Instead this just runs LUAMAIN_SCRIPT_PATH -- a real .lua file on
// the EXT2 disk image (see install-main.sh) -- as the program, the
// same fixed-script-path shape python.c already uses for
// PYMAIN_SCRIPT_PATH. Replace that file's content to run something
// else; nothing here needs to change.
//
// Unlike the CPython port, this needed no shims at all beyond this one
// file: Lua's own src/*.c (minus lua.c/luac.c, neither built here)
// compiles cleanly as-is against musl with no LUA_USE_* macro defined
// at all (see setup.sh's LUA_CFLAGS comment) -- every POSIX-only code
// path in loadlib.c/loslib.c/liolib.c/lauxlib.c (dlopen-based dynamic
// libraries, mkstemp-based os.tmpname, sys/wait.h's pclose status
// macros) is already `#if defined(LUA_USE_POSIX)`-gated by upstream
// Lua itself, and simply compiles out under the default ISO C
// fallback used here instead.
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// The program to run -- a real file on the EXT2 disk image, not baked
// into this binary. install-main.sh writes it there.
#define LUAMAIN_SCRIPT_PATH "/lualib/main.lua"

// Prints a Lua error value (left on top of the stack by a failed
// luaL_loadfile/lua_pcall) the same way lua.c's own l_message/report
// do: to stderr, prefixed with "lua: ".
static void report_error(lua_State *L)
{
	const char *msg = lua_tostring(L, -1);
	if (msg == NULL) {
		msg = "(error message not a string)";
	}
	fprintf(stderr, "lua: %s\n", msg);
	lua_pop(L, 1);
}

int main(void)
{
	lua_State *L = luaL_newstate();
	if (L == NULL) {
		fprintf(stderr, "lua.c: luaL_newstate() failed (out of memory)\n");
		exit(1);
	}

	luaL_openlibs(L);

	int rc = 1;
	if (luaL_loadfile(L, LUAMAIN_SCRIPT_PATH) != LUA_OK) {
		report_error(L);
	} else if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
		report_error(L);
	} else {
		rc = 0;
	}

	lua_close(L);
	exit(rc);
}
