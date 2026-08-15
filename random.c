// random.c -- exercise getrandom() end to end: musl's getrandom() ->
// posix_shim.c's SYS_getrandom -> sys_getrandom()'s RDRAND-with-RDTSC-
// fallback. See posix_shim.c's "getrandom()" section and OPENISSUES.md's
// "Missing common syscalls" entry.
// build with build-app.sh
//
// This is also a valid *nix program of course.

#include <stdio.h>
#include <string.h>
#include <sys/random.h>

static void print_hex(const unsigned char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		printf("%02x", buf[i]);
	printf("\n");
}

int main(void)
{
	// A handful of same-size draws should never repeat and should
	// never come back all-zero -- either would point at a broken
	// RDRAND/RDTSC path rather than real entropy.
	unsigned char bufs[3][16];
	for (int i = 0; i < 3; i++) {
		ssize_t n = getrandom(bufs[i], sizeof(bufs[i]), 0);
		if (n != (ssize_t)sizeof(bufs[i])) {
			printf("getrandom() call %d: short/failed return %zd\n", i, n);
			return 1;
		}
		printf("draw %d: ", i);
		print_hex(bufs[i], sizeof(bufs[i]));
	}

	if (memcmp(bufs[0], bufs[1], sizeof(bufs[0])) == 0 ||
	    memcmp(bufs[1], bufs[2], sizeof(bufs[1])) == 0) {
		printf("FAIL: two draws came back identical\n");
		return 1;
	}

	unsigned char zero[16] = {0};
	if (memcmp(bufs[0], zero, sizeof(zero)) == 0) {
		printf("FAIL: a draw came back all-zero\n");
		return 1;
	}

	// Odd/small/large-relative-to-a-single-rdrand-word lengths, and
	// the GRND_RANDOM/GRND_NONBLOCK flags (accepted and ignored --
	// see posix_shim.c) -- exercises sys_getrandom()'s chunking loop.
	unsigned char small[1], odd[7], big[257];
	getrandom(small, sizeof(small), GRND_NONBLOCK);
	getrandom(odd, sizeof(odd), GRND_RANDOM);
	ssize_t n = getrandom(big, sizeof(big), 0);
	printf("1-byte draw:   ");
	print_hex(small, sizeof(small));
	printf("7-byte draw:   ");
	print_hex(odd, sizeof(odd));
	printf("257-byte draw: %zd bytes returned\n", n);

	printf("PASS\n");
	return 0;
}
