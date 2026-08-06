// clock.c -- print the current wall-clock time and time elapsed since
// boot, two ways: through musl's time()/clock_gettime() (proving the
// musl -> posix_shim -> b_system(WALLCLOCK/TIMECOUNTER) path works end
// to end) and through a direct b_system(WALLCLOCK, 0, 0) call (proving
// the new kernel function itself). See posix_shim.c's "Time" section.
// build with build-app.sh
//
// This is also a valid *nix program of course.

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "port/libBareMetal.h"

int main(void)
{
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);

	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	printf("time():           %s UTC (%lld seconds since epoch)\n",
	       buf, (long long)now);

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("clock_gettime(CLOCK_MONOTONIC): %lld.%09ld seconds since boot\n",
	       (long long)ts.tv_sec, ts.tv_nsec);

	u64 wallclock = b_system(WALLCLOCK, 0, 0);
	printf("b_system(WALLCLOCK, 0, 0):      %llu seconds since epoch\n",
	       (unsigned long long)wallclock);

	// Exercises nanosleep() (via musl's sleep()) -- see posix_shim.c's
	// sys_nanosleep().
	for (int i = 1; i <= 5; i++) {
		sleep(1);
		printf("%d\n", i);
	}

	return 0;
}
