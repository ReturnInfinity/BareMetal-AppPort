// mod_extern.c -- exercises calls out through dl_exports[] (GLOB_DAT/
// JUMP_SLOT relocations against host functions dlfcn_shim.c's
// apply_relocs() resolves via dl_export_lookup(), not against anything
// defined in this module itself). Deliberately calls a spread of the
// table's entries -- malloc/free/memcpy/memset/memcmp/strlen/strcmp --
// so a single missing or misresolved export table entry shows up as
// this module misbehaving rather than failing to load at all.
//
// Build with ./build-module.sh dlopen_modules/mod_extern.c -o build/mod_extern.so

#include <stddef.h>

extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);

int roundtrip_via_host(void)
{
	char *buf = malloc(64);
	if (!buf)
		return -1;

	memset(buf, 'x', 64);
	if (buf[0] != 'x' || buf[63] != 'x') {
		free(buf);
		return -2;
	}

	const char *msg = "hello from mod_extern";
	memcpy(buf, msg, strlen(msg) + 1);
	if (strcmp(buf, msg) != 0) {
		free(buf);
		return -3;
	}
	if (memcmp(buf, msg, strlen(msg)) != 0) {
		free(buf);
		return -4;
	}

	free(buf);
	return 0;
}
