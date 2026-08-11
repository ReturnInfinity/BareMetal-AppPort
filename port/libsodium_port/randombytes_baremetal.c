// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// randombytes_baremetal.c -- libsodium's randombytes_implementation hook.
// There's no /dev/urandom and no getrandom() syscall here (see
// OPENISSUES.md), so libsodium's own randombytes_sysrandom.c (which opens
// /dev/urandom, or falls back to getrandom()/getentropy()) is never
// compiled into this port at all (see setup.sh's "Building libsodium"
// step) -- this takes its place, the exact same way port/mbedtls_port/
// entropy_hardware_poll.c stands in for mbedTLS's platform entropy source:
// RDRAND, with an RDTSC-based fallback for the (today, vanishingly rare)
// case a CPU lacks it.
//
// This is named/typed to be a drop-in for randombytes_sysrandom.c rather
// than going through randombytes_set_implementation() at runtime: it
// defines the exact global libsodium's own src/libsodium/randombytes/
// randombytes.c looks for by default (RANDOMBYTES_DEFAULT_IMPLEMENTATION,
// see randombytes.h's decl in randombytes_sysrandom.h), so no app code
// (see sodium.c) has to call randombytes_set_implementation() itself --
// sodium_init() picks this up the same way it would pick up the real
// sysrandom implementation on a normal OS.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#include "randombytes.h"

static int has_rdrand(void)
{
	unsigned int eax, ebx, ecx, edx;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
	return (ecx >> 30) & 1;
}

// Same RDRAND-with-retry, RDTSC-mixing-fallback strategy as
// entropy_hardware_poll.c's mbedtls_hardware_poll() -- see that file's
// comment for the reasoning. Fills len bytes, 8 (or a final partial
// chunk) at a time.
static void baremetal_buf(void *const buf, const size_t len)
{
	unsigned char *out = (unsigned char *)buf;
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
			unsigned int lo, hi;
			__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
			v = ((unsigned long)hi << 32 | lo) ^ (unsigned long)out ^ (unsigned long)n;
		}

		size_t chunk = len - n < sizeof(v) ? len - n : sizeof(v);
		__builtin_memcpy(out + n, &v, chunk);
		n += chunk;
	}
}

static uint32_t baremetal_random(void)
{
	uint32_t v;

	baremetal_buf(&v, sizeof v);
	return v;
}

static const char *baremetal_implementation_name(void)
{
	return "baremetal-rdrand";
}

struct randombytes_implementation randombytes_sysrandom_implementation = {
	.implementation_name = baremetal_implementation_name,
	.random               = baremetal_random,
	.stir                 = NULL,
	.uniform              = NULL,
	.buf                  = baremetal_buf,
	.close                = NULL,
};

// =============================================================================
// EOF
