#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

// lwIP port for the BareMetal-Firecracker exokernel. Almost none of
// this file is needed: lwIP's own defaults (lwip/arch.h) already
// cover stdint/stddef/inttypes/limits types and GCC struct packing
// with nothing to do here. The two things that genuinely have no
// usable default are LWIP_RAND() (musl's rand() works fine once
// seeded -- see net_glue.c) and LWIP_PLATFORM_ASSERT (kept close to
// upstream's default, just without fflush(), which is meaningless
// for our single always-line-buffered stdout).

#include <stdlib.h>
#include <stdio.h>

unsigned int bmos_lwip_rand(void);
#define LWIP_RAND() (bmos_lwip_rand())

#define LWIP_PLATFORM_ASSERT(x) do { \
	printf("lwIP assertion \"%s\" failed at line %d in %s\n", x, __LINE__, __FILE__); \
	abort(); \
} while (0)

#endif
