// signals.c -- exercises the signal delivery added in port/thread_shim.c's
// "Signals" section: SYS_rt_sigaction/rt_sigprocmask/kill/tkill/tgkill,
// wired up to run a real handler off the same CALLBACK_TIMER injection
// point thread_shim.c already uses for preemption, plus the resulting
// -EINTR out of blocking primitives (thread_shim_futex()'s FUTEX_WAIT,
// sleep_until_ns()). See thread_shim.c's own comment for the design and
// its honest limits (tick-granularity delivery, no hardware-fault-
// derived signals, no real ucontext_t).
// build with build-app.sh
//
// Deliberately does NOT test a signal's default "terminate the process"
// disposition -- there's no fork() on this port to isolate that in a
// child, and killing this test process would just end the run early.
// That path (thread_shim_terminate_process(), reusing sys_exit()'s own
// teardown) is exercised implicitly by real Linux running this same
// source as an ordinary program if it's ever run there.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

static int g_failures = 0;

#define CHECK(cond, ...) do { \
	if (cond) { \
		printf("  PASS: " __VA_ARGS__); \
		printf("\n"); \
	} else { \
		printf("  FAIL: " __VA_ARGS__); \
		printf("\n"); \
		g_failures++; \
	} \
} while (0)

static pthread_attr_t g_small_attr;
#define SMALL_STACK (32 * 1024)

static void init_small_attr(void)
{
	pthread_attr_init(&g_small_attr);
	pthread_attr_setstacksize(&g_small_attr, SMALL_STACK);
	pthread_attr_setguardsize(&g_small_attr, 0);
}

// -----------------------------------------------------------------------
// A caught signal actually runs its handler -- raise() on self.
// -----------------------------------------------------------------------

static volatile sig_atomic_t g_handler_hits;
static volatile int g_handler_last_sig;

static void basic_handler(int sig)
{
	g_handler_hits++;
	g_handler_last_sig = sig;
}

static void test_raise_self(void)
{
	printf("-- sigaction + raise() on self --\n");

	struct sigaction sa = {0};
	sa.sa_handler = basic_handler;
	CHECK(sigaction(SIGUSR1, &sa, 0) == 0, "sigaction(SIGUSR1) installs a handler");

	g_handler_hits = 0;
	g_handler_last_sig = 0;
	CHECK(raise(SIGUSR1) == 0, "raise(SIGUSR1) succeeds");

	// Delivery happens on the next timer tick (~1ms), not synchronously
	// inside raise() itself -- see thread_shim.c's "Signals" section.
	usleep(20000);
	CHECK(g_handler_hits == 1, "handler ran exactly once (ran %d times)", g_handler_hits);
	CHECK(g_handler_last_sig == SIGUSR1, "handler saw the right signal number (got %d)", g_handler_last_sig);

	signal(SIGUSR1, SIG_DFL);
}

// -----------------------------------------------------------------------
// pthread_kill() reaches a *different*, currently-running thread.
// -----------------------------------------------------------------------

static volatile sig_atomic_t g_cross_hits;
static volatile pthread_t g_target_self;

static void cross_handler(int sig)
{
	(void)sig;
	g_cross_hits++;
}

static void *spin_fn(void *arg)
{
	(void)arg;
	g_target_self = pthread_self();
	// Busy-spin (stays T_RUNNING the whole time) so the handler fires
	// via the ordinary running-thread path in thread_shim_timer_tick_c(),
	// not the T_BLOCKED wake path -- see that file's comment.
	while (g_cross_hits == 0)
		;
	return 0;
}

static void test_cross_thread_kill(void)
{
	printf("-- pthread_kill() targets a different running thread --\n");

	struct sigaction sa = {0};
	sa.sa_handler = cross_handler;
	sigaction(SIGUSR2, &sa, 0);

	g_cross_hits = 0;
	g_target_self = 0;
	pthread_t t;
	pthread_create(&t, &g_small_attr, spin_fn, 0);

	usleep(20000); // let it actually start spinning first
	CHECK(pthread_kill(t, SIGUSR2) == 0, "pthread_kill() on a running peer succeeds");

	pthread_join(t, 0);
	CHECK(g_cross_hits >= 1, "the *target* thread's handler ran (hits=%d), not the caller's", g_cross_hits);

	signal(SIGUSR2, SIG_DFL);
}

// -----------------------------------------------------------------------
// sigprocmask -- a blocked signal doesn't fire until unblocked.
// -----------------------------------------------------------------------

static void test_sigprocmask(void)
{
	printf("-- sigprocmask blocks/unblocks delivery --\n");

	struct sigaction sa = {0};
	sa.sa_handler = basic_handler;
	sigaction(SIGUSR1, &sa, 0);

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	CHECK(sigprocmask(SIG_BLOCK, &set, 0) == 0, "sigprocmask(SIG_BLOCK) succeeds");

	g_handler_hits = 0;
	raise(SIGUSR1);
	usleep(20000);
	CHECK(g_handler_hits == 0, "handler did NOT run while SIGUSR1 is blocked (ran %d times)", g_handler_hits);

	CHECK(sigprocmask(SIG_UNBLOCK, &set, 0) == 0, "sigprocmask(SIG_UNBLOCK) succeeds");
	usleep(20000);
	CHECK(g_handler_hits == 1, "handler ran once unblocked (ran %d times)", g_handler_hits);

	signal(SIGUSR1, SIG_DFL);
}

// -----------------------------------------------------------------------
// A signal interrupts a blocking nanosleep() with EINTR + real remaining
// time, rather than always running to completion.
// -----------------------------------------------------------------------

static void *delayed_kill_fn(void *arg)
{
	pthread_t target = *(pthread_t *)arg;
	usleep(50000); // let the sleeper actually get into nanosleep() first
	pthread_kill(target, SIGUSR1);
	return 0;
}

static void test_nanosleep_eintr(void)
{
	printf("-- signal interrupts a blocking nanosleep() --\n");

	struct sigaction sa = {0};
	sa.sa_handler = basic_handler; // no SA_RESTART -- see thread_shim.c's comment
	sigaction(SIGUSR1, &sa, 0);
	g_handler_hits = 0;

	pthread_t self = pthread_self();
	pthread_t killer;
	pthread_create(&killer, &g_small_attr, delayed_kill_fn, &self);

	struct timespec req = { .tv_sec = 1, .tv_nsec = 0 }, rem = {0};
	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);
	int rc = nanosleep(&req, &rem);
	clock_gettime(CLOCK_MONOTONIC, &after);
	double elapsed_ms = (after.tv_sec - before.tv_sec) * 1000.0
	                  + (after.tv_nsec - before.tv_nsec) / 1e6;

	pthread_join(killer, 0);

	CHECK(rc == -1 && errno == EINTR, "nanosleep() returns -1/EINTR (rc=%d, errno=%d)", rc, errno);
	CHECK(g_handler_hits == 1, "the SIGUSR1 handler ran before nanosleep() returned (ran %d times)", g_handler_hits);
	CHECK(elapsed_ms < 900.0, "woke early, well before the full 1s (%.1fms elapsed)", elapsed_ms);
	CHECK(rem.tv_sec > 0 || rem.tv_nsec > 0, "*rem reports real remaining time, not zeroed (%lds %ldns)",
	      (long)rem.tv_sec, (long)rem.tv_nsec);

	signal(SIGUSR1, SIG_DFL);
}

// -----------------------------------------------------------------------
// SA_RESTART suppresses the EINTR a plain handler would cause.
// -----------------------------------------------------------------------

static void test_sa_restart(void)
{
	printf("-- SA_RESTART signal does not interrupt nanosleep() --\n");

	struct sigaction sa = {0};
	sa.sa_handler = basic_handler;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, 0);
	g_handler_hits = 0;

	pthread_t self = pthread_self();
	pthread_t killer;
	pthread_create(&killer, &g_small_attr, delayed_kill_fn, &self);

	struct timespec req = { .tv_sec = 0, .tv_nsec = 200000000 }, rem = {0}; // 200ms
	int rc = nanosleep(&req, &rem);

	pthread_join(killer, 0);

	CHECK(rc == 0, "nanosleep() still reports success (rc=%d)", rc);
	CHECK(g_handler_hits == 1, "the SA_RESTART handler still ran (ran %d times)", g_handler_hits);

	signal(SIGUSR1, SIG_DFL);
}

// -----------------------------------------------------------------------
// pthread_cancel() targeting a thread parked in pthread_cond_wait() --
// exercises the WOKEN_INTR path in thread_shim.c's futex_wait(), which
// is what actually lets cross-thread cancellation take effect on a
// genuinely blocked thread on this port (see thread_shim.c's "Signals"
// section for why the real PC-redirect trick musl's cancel_handler()
// tries first doesn't apply here, and isn't needed).
// -----------------------------------------------------------------------

static pthread_mutex_t g_cancel_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cancel_cv = PTHREAD_COND_INITIALIZER;

static void *cancel_target_fn(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_cancel_mutex);
	// Nobody will ever signal this -- the only way out is cancellation.
	pthread_cond_wait(&g_cancel_cv, &g_cancel_mutex);
	pthread_mutex_unlock(&g_cancel_mutex);
	return 0;
}

static void test_cancel_blocked_thread(void)
{
	printf("-- pthread_cancel() reaches a thread blocked in cond_wait --\n");

	pthread_t t;
	pthread_create(&t, &g_small_attr, cancel_target_fn, 0);
	usleep(30000); // let it actually get into the wait first

	CHECK(pthread_cancel(t) == 0, "pthread_cancel() on a blocked peer succeeds");

	void *res = 0;
	int rc = pthread_join(t, &res);
	CHECK(rc == 0 && res == PTHREAD_CANCELED,
	      "the blocked thread actually exited with PTHREAD_CANCELED (rc=%d, res=%p)", rc, res);
}

// -----------------------------------------------------------------------

int main(void)
{
	printf("signals.c -- BareMetal-AppPort signal delivery test\n\n");

	init_small_attr();

	test_raise_self();
	test_cross_thread_kill();
	test_sigprocmask();
	test_nanosleep_eintr();
	test_sa_restart();
	test_cancel_blocked_thread();

	pthread_attr_destroy(&g_small_attr);

	printf("\n%s: %d failure(s)\n", g_failures ? "RESULT" : "RESULT (all passed)", g_failures);
	return g_failures ? 1 : 0;
}
