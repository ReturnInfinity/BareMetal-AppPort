// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// sqlite.c -- sqlite.app: SQLite's own command-line shell
// (shell.c, shipped with the amalgamation), run against a SQL script
// on the EXT2 disk image. Same pattern as python.app/lua.app: the
// program is a file, SQLITE_SCRIPT_PATH, that port/sqlite_port/
// install-main.sh writes there, and this runs it as if by
//
//   HOME=/sqlite sqlite3 -echo SQLITE_DB_PATH ".read SQLITE_SCRIPT_PATH"
//
// printing each statement and its results to the console. HOME is set
// so the shell finds a /sqlite/.sqliterc if there is one, instead of
// warning that there's no home directory at all. The database
// stays on the disk image between runs. Any VM arguments (2-run.sh a b
// ..., see port/crt0.c) go in as shell options ahead of the database
// name, so e.g. `./2-run.sh -box` or `-header -csv` changes the output
// format.
//
// shell.c is #included rather than linked as its own object so it's
// compiled with the same sqlite_baremetal_config.h options sqlite3.o
// was (setup.sh's SQLITE_CFLAGS) -- several of them (SQLITE_OMIT_*)
// change which shell features exist -- and so its main() can be renamed
// out of the way of this one. Shell commands that need a process or a
// terminal (.system/.shell, .once -e, the interactive prompt) don't
// work here.
// =============================================================================

#include "sqlite_baremetal_config.h"

#define main sqlite3_shell_main
#include "shell.c"
#undef main

#define SQLITE_SCRIPT_PATH "/sqlite/main.sql"
#define SQLITE_DB_PATH     "/sqlite/main.db"
#define SQLITE_HOME        "/sqlite"
#define SQLITE_MAX_ARGS    32

int main(int argc, char **argv)
{
	char *args[SQLITE_MAX_ARGS + 1];
	int n = 0;

	setenv("HOME", SQLITE_HOME, 1);

	args[n++] = "sqlite3";
	args[n++] = "-echo";
	for (int i = 1; i < argc && n < SQLITE_MAX_ARGS - 2; i++)
		args[n++] = argv[i];
	args[n++] = SQLITE_DB_PATH;
	args[n++] = ".read " SQLITE_SCRIPT_PATH;
	args[n] = NULL;

	return sqlite3_shell_main(n, args);
}
