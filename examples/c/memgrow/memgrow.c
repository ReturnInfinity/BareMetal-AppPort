// memgrow.c -- exercise memory hot-plug (virtio-mem).
//
// Boots on a tiny VM (baremetal.sh's default MEMSIZE=4) and allocates far
// more than that. Every malloc() that outgrows the app's RAM window makes
// the posix shim ask the kernel for more (b_system(GROW_MEMORY)), which
// hot-plugs blocks from Firecracker and appends them to the window, so
// b_system(FREE_MEMORY) climbs as the allocations go through. Each chunk
// is written and read back in full so the new memory is really touched.
//
// Run with e.g. `./1-build.sh examples/c/memgrow/memgrow.c && ./2-run.sh`.
// An optional first argument sets the total to allocate in MiB
// (default 64, capped by MEMHOTPLUG_MAX in baremetal.sh).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 1-build.sh mirrors this file to BareMetal-AppPort/examples/c/memgrow/
// before compiling, so the kernel API header is found relative to there
#include "../../../port/libBareMetal.h"

#define CHUNK_MIB 16

int main(int argc, char **argv)
{
	unsigned long total_mib = 64;
	if (argc > 1 && atol(argv[1]) > 0)
		total_mib = (unsigned long)atol(argv[1]);

	printf("memgrow: app RAM at start: %lu MiB\n",
	       (unsigned long)b_system(FREE_MEMORY, 0, 0));

	unsigned long allocated = 0;
	unsigned int chunks = 0;
	unsigned char *ptrs[256];

	while (allocated < total_mib && chunks < 256) {
		size_t sz = (size_t)CHUNK_MIB * 1024 * 1024;
		unsigned char *p = malloc(sz);
		if (!p) {
			printf("memgrow: malloc(%u MiB) failed after %lu MiB "
			       "(app RAM now %lu MiB) -- hot-plug budget exhausted?\n",
			       CHUNK_MIB, allocated,
			       (unsigned long)b_system(FREE_MEMORY, 0, 0));
			break;
		}

		// Touch every byte so unbacked/unmapped memory would fault here
		memset(p, (int)(0xA5 ^ chunks), sz);
		for (size_t i = 0; i < sz; i += 4096) {
			if (p[i] != (unsigned char)(0xA5 ^ chunks)) {
				printf("memgrow: readback mismatch at chunk %u offset %zu\n",
				       chunks, i);
				return 1;
			}
		}

		ptrs[chunks++] = p;
		allocated += CHUNK_MIB;
		printf("memgrow: %3lu MiB allocated, app RAM now %lu MiB\n",
		       allocated, (unsigned long)b_system(FREE_MEMORY, 0, 0));
	}

	// Make sure the earliest chunks survived all the growth
	for (unsigned int c = 0; c < chunks; c++) {
		if (ptrs[c][0] != (unsigned char)(0xA5 ^ c) ||
		    ptrs[c][CHUNK_MIB * 1024 * 1024 - 1] != (unsigned char)(0xA5 ^ c)) {
			printf("memgrow: chunk %u corrupted\n", c);
			return 1;
		}
		free(ptrs[c]);
	}

	printf("memgrow: done, %lu MiB allocated and verified, app RAM %lu MiB\n",
	       allocated, (unsigned long)b_system(FREE_MEMORY, 0, 0));
	return 0;
}
