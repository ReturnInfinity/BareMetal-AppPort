// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// lighttpd.c -- lighttpd.app's entry point. lighttpd's own server.c is
// compiled with main renamed away, so its server_main() is called from
// here instead, with the arguments a BareMetal boot can't easily pass:
// stay in the foreground (-D -- there's no fork() to daemonize with
// anyway) and read LIGHTTPD_CONF_PATH from the EXT2 disk image, the
// same fixed-path convention python.app/lua.app use for their script.
// Any VM arguments (2-run.sh a b ..., see port/crt0.c) are appended,
// so e.g. "-tt" still works to just check the config.
// =============================================================================

#include <stdio.h>

#define LIGHTTPD_CONF_PATH "/lighttpd/lighttpd.conf"
#define LIGHTTPD_MAX_ARGS  16

int server_main(int argc, char **argv);

int main(int argc, char **argv)
{
	char *args[LIGHTTPD_MAX_ARGS + 1];
	int n = 0;

	args[n++] = "lighttpd";
	args[n++] = "-D";
	args[n++] = "-f";
	args[n++] = LIGHTTPD_CONF_PATH;
	for (int i = 1; i < argc && n < LIGHTTPD_MAX_ARGS; i++)
		args[n++] = argv[i];
	args[n] = NULL;

	return server_main(n, args);
}
