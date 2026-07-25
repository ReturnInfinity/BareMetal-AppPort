// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// entropy_hardware_poll.c -- mbedTLS's MBEDTLS_ENTROPY_HARDWARE_ALT hook
// (see mbedtls_config.h). There's no /dev/urandom and no getrandom()
// syscall here (see OPENISSUES.md), so mbedTLS's own built-in platform
// entropy source is disabled (MBEDTLS_NO_PLATFORM_ENTROPY) and this
// takes its place, the same way crt0.c's fill_random() seeds musl's
// stack-protector/mallocng entropy: RDRAND, with an RDTSC-based
// fallback for the (today, vanishingly rare) case a CPU lacks it.
// =============================================================================

#include <stddef.h>

static int has_rdrand(void)
{
	unsigned int eax, ebx, ecx, edx;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
	return (ecx >> 30) & 1;
}

// mbedTLS calls this in a loop as needed (e.g. to refill the CTR_DRBG),
// not just once at startup like crt0.c's fill_random(), so unlike that
// one this doesn't need a fixed 16-byte buffer -- it just fills
// whatever len the caller asks for, 8 bytes (or less, for a final
// partial chunk) at a time.
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	(void)data;

	int rdrand_ok = has_rdrand();
	size_t n = 0;

	while (n < len) {
		unsigned long v = 0;
		int ok = 0;

		if (rdrand_ok) {
			for (int tries = 0; tries < 10 && !ok; tries++)
				__asm__ volatile ("rdrand %0\n\tsetc %b1" : "=r"(v), "=q"(ok) :: "cc");
		}

		if (!ok) {
			// No RDRAND (or it kept failing): fall back to a
			// non-cryptographic mix, same as crt0.c's
			// fill_random(). Degrades the strength of whatever
			// this seeds, but every CPU capable of running this
			// port at all (x86-64) has shipped with RDRAND for
			// well over a decade, so this path is a defensive
			// fallback, not the expected case.
			unsigned int lo, hi;
			__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
			v = ((unsigned long)hi << 32 | lo) ^ (unsigned long)output ^ (unsigned long)n;
		}

		size_t chunk = len - n < sizeof(v) ? len - n : sizeof(v);
		__builtin_memcpy(output + n, &v, chunk);
		n += chunk;
	}

	*olen = n;
	return 0;
}

// =============================================================================
// EOF
