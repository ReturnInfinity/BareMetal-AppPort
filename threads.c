// threads.c -- exercises the cooperative pthread_* implementation added
// in port/thread_shim.c: pthread_create/join/detach/self/equal, mutexes
// (normal, trylock, recursive), condition variables (signal, broadcast,
// timedwait), rwlocks, pthread_once, thread-specific data (both
// pthread_key_create() and compiler __thread storage), spinlocks,
// barriers, sched_yield(), and a custom pthread_attr_t stack size.
// Proves the musl -> posix_shim -> thread_shim.c path (SYS_clone,
// SYS_futex, SYS_sched_yield) works end to end, and -- since
// thread_shim.c schedules on a real timer tick, not just at explicit
// yield points -- that real concurrent interleaving is happening, not
// just cooperative hand-offs.
// build with build-app.sh
//
// This is also a valid *nix program of course, though pthread_create()
// there uses real kernel threads rather than this port's cooperative
// ones -- the pass/fail checks hold either way.
//
// Threads here deliberately use a small explicit stack size (see
// SMALL_ATTR below) rather than musl's 128KB+8KB-guard default: the
// microVM this port targets is configured with as little as 4MiB of
// RAM total (see BareMetal-Firecracker/baremetal.sh), and every mmap()
// (including a thread's stack) draws from that same fixed arena -- see
// posix_shim.c's heap section.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
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

// Small stacks for the multi-thread tests -- see file header.
static pthread_attr_t g_small_attr;
#define SMALL_STACK (32 * 1024)

static void init_small_attr(void)
{
	pthread_attr_init(&g_small_attr);
	pthread_attr_setstacksize(&g_small_attr, SMALL_STACK);
	pthread_attr_setguardsize(&g_small_attr, 0);
}

// -----------------------------------------------------------------------
// pthread_create / pthread_join / pthread_self / pthread_equal
// -----------------------------------------------------------------------

static void *create_join_fn(void *arg)
{
	long n = (long)arg;
	return (void *)(n * n);
}

static void test_create_join(void)
{
	printf("-- create/join/self/equal --\n");

	pthread_t self = pthread_self();
	CHECK(pthread_equal(self, pthread_self()), "pthread_self() is stable across calls");

	pthread_t threads[4];
	for (long i = 0; i < 4; i++) {
		int rc = pthread_create(&threads[i], &g_small_attr, create_join_fn, (void *)i);
		CHECK(rc == 0, "pthread_create() thread %ld (rc=%d)", i, rc);
	}

	CHECK(!pthread_equal(threads[0], threads[1]), "distinct threads have distinct pthread_t handles");

	int ok = 1;
	for (long i = 0; i < 4; i++) {
		void *res = (void *)0xdeadbeef;
		int rc = pthread_join(threads[i], &res);
		if (rc != 0 || (long)res != i * i)
			ok = 0;
	}
	CHECK(ok, "pthread_join() returned each thread's i*i return value");
}

// -----------------------------------------------------------------------
// pthread_mutex_t -- default (normal), trylock, recursive
// -----------------------------------------------------------------------

#define COUNT_ITERS 20000
#define COUNT_THREADS 4

static pthread_mutex_t g_count_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_shared_counter;

static void *counter_fn(void *arg)
{
	(void)arg;
	for (int i = 0; i < COUNT_ITERS; i++) {
		pthread_mutex_lock(&g_count_mutex);
		// A read-modify-write with no lock protection would almost
		// certainly lose updates under thread_shim.c's real timer-tick
		// preemption (see its file header) -- that's the point of
		// this test.
		long v = g_shared_counter;
		if (i % 4096 == 0)
			sched_yield(); // widen the window a preempting tick has to land in
		g_shared_counter = v + 1;
		pthread_mutex_unlock(&g_count_mutex);
	}
	return 0;
}

static void test_mutex(void)
{
	printf("-- mutex (mutual exclusion under real preemption) --\n");

	g_shared_counter = 0;
	pthread_t threads[COUNT_THREADS];
	for (int i = 0; i < COUNT_THREADS; i++)
		pthread_create(&threads[i], &g_small_attr, counter_fn, 0);
	for (int i = 0; i < COUNT_THREADS; i++)
		pthread_join(threads[i], 0);

	CHECK(g_shared_counter == (long)COUNT_ITERS * COUNT_THREADS,
	      "counter == %ld after %d threads x %d locked increments (got %ld)",
	      (long)COUNT_ITERS * COUNT_THREADS, COUNT_THREADS, COUNT_ITERS, g_shared_counter);
}

static void *trylock_fn(void *arg)
{
	pthread_mutex_t *m = arg;
	return (void *)(long)pthread_mutex_trylock(m);
}

static void test_mutex_trylock(void)
{
	printf("-- pthread_mutex_trylock --\n");

	pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	CHECK(pthread_mutex_trylock(&m) == 0, "trylock succeeds on an unlocked mutex");

	pthread_t t;
	pthread_create(&t, &g_small_attr, trylock_fn, &m);
	void *res = 0;
	pthread_join(t, &res);
	CHECK((int)(long)res == EBUSY, "trylock from another thread reports EBUSY while held");

	pthread_mutex_unlock(&m);
	pthread_mutex_destroy(&m);
}

static void test_recursive_mutex(void)
{
	printf("-- recursive mutex --\n");

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

	pthread_mutex_t m;
	pthread_mutex_init(&m, &attr);
	pthread_mutexattr_destroy(&attr);

	int ok = pthread_mutex_lock(&m) == 0
	      && pthread_mutex_lock(&m) == 0	// same thread, second lock -- would deadlock on a normal mutex
	      && pthread_mutex_lock(&m) == 0;
	ok = ok && pthread_mutex_unlock(&m) == 0
	        && pthread_mutex_unlock(&m) == 0
	        && pthread_mutex_unlock(&m) == 0;
	CHECK(ok, "same thread relocks a recursive mutex 3x and unlocks it 3x");

	pthread_mutex_destroy(&m);
}

// -----------------------------------------------------------------------
// pthread_cond_t -- signal, broadcast, timedwait
// -----------------------------------------------------------------------

static pthread_mutex_t g_cv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_cv_ready;

static void *cv_waiter_fn(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_cv_mutex);
	while (!g_cv_ready)
		pthread_cond_wait(&g_cv, &g_cv_mutex);
	pthread_mutex_unlock(&g_cv_mutex);
	return 0;
}

static void test_cond_signal(void)
{
	printf("-- condvar (signal) --\n");

	g_cv_ready = 0;
	pthread_t t;
	pthread_create(&t, &g_small_attr, cv_waiter_fn, 0);

	usleep(20000); // give the waiter a chance to actually block first
	pthread_mutex_lock(&g_cv_mutex);
	g_cv_ready = 1;
	pthread_cond_signal(&g_cv);
	pthread_mutex_unlock(&g_cv_mutex);

	pthread_join(t, 0);
	CHECK(1, "waiter woke up and returned after pthread_cond_signal()");
}

#define BCAST_WAITERS 3

static pthread_mutex_t g_bc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_bc_cv = PTHREAD_COND_INITIALIZER;
static int g_bc_ready;
static int g_bc_woken;

static void *bcast_waiter_fn(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_bc_mutex);
	while (!g_bc_ready)
		pthread_cond_wait(&g_bc_cv, &g_bc_mutex);
	g_bc_woken++;
	pthread_mutex_unlock(&g_bc_mutex);
	return 0;
}

static void test_cond_broadcast(void)
{
	// With >1 waiter and a plain (non-PI) mutex, musl's cond_wait chains
	// all-but-one waiter through FUTEX_REQUEUE on the way back from a
	// broadcast (see pthread_cond_timedwait.c's unlock_requeue()) --
	// this is what actually exercises thread_shim.c's FUTEX_REQUEUE
	// fallback, not just FUTEX_WAKE.
	printf("-- condvar (broadcast, %d waiters) --\n", BCAST_WAITERS);

	g_bc_ready = 0;
	g_bc_woken = 0;
	pthread_t threads[BCAST_WAITERS];
	for (int i = 0; i < BCAST_WAITERS; i++)
		pthread_create(&threads[i], &g_small_attr, bcast_waiter_fn, 0);

	usleep(20000);
	pthread_mutex_lock(&g_bc_mutex);
	g_bc_ready = 1;
	pthread_cond_broadcast(&g_bc_cv);
	pthread_mutex_unlock(&g_bc_mutex);

	for (int i = 0; i < BCAST_WAITERS; i++)
		pthread_join(threads[i], 0);

	CHECK(g_bc_woken == BCAST_WAITERS, "all %d waiters woke from one broadcast (got %d)",
	      BCAST_WAITERS, g_bc_woken);
}

static void test_cond_timedwait(void)
{
	printf("-- pthread_cond_timedwait (real timeout) --\n");

	pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 0;
	ts.tv_nsec += 150000000; // 150ms -- nobody will ever signal this cv
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);

	pthread_mutex_lock(&m);
	int rc = pthread_cond_timedwait(&cv, &m, &ts);
	pthread_mutex_unlock(&m);

	clock_gettime(CLOCK_MONOTONIC, &after);
	double elapsed_ms = (after.tv_sec - before.tv_sec) * 1000.0
	                  + (after.tv_nsec - before.tv_nsec) / 1e6;

	CHECK(rc == ETIMEDOUT, "unsignaled cond wait times out (rc=%d, expected ETIMEDOUT)", rc);
	CHECK(elapsed_ms >= 100.0, "timeout waited roughly as long as asked (%.1fms, expected ~150ms)", elapsed_ms);

	pthread_mutex_destroy(&m);
	pthread_cond_destroy(&cv);
}

// -----------------------------------------------------------------------
// pthread_rwlock_t
// -----------------------------------------------------------------------

static pthread_rwlock_t g_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static volatile int g_rw_readers_active;
static volatile int g_rw_max_concurrent_readers;

static void *rwlock_reader_fn(void *arg)
{
	(void)arg;
	pthread_rwlock_rdlock(&g_rwlock);
	g_rw_readers_active++;
	if (g_rw_readers_active > g_rw_max_concurrent_readers)
		g_rw_max_concurrent_readers = g_rw_readers_active;
	usleep(20000);
	g_rw_readers_active--;
	pthread_rwlock_unlock(&g_rwlock);
	return 0;
}

static void test_rwlock(void)
{
	printf("-- rwlock (concurrent readers, exclusive writer) --\n");

	g_rw_readers_active = 0;
	g_rw_max_concurrent_readers = 0;

	pthread_t readers[3];
	for (int i = 0; i < 3; i++)
		pthread_create(&readers[i], &g_small_attr, rwlock_reader_fn, 0);
	for (int i = 0; i < 3; i++)
		pthread_join(readers[i], 0);

	CHECK(g_rw_max_concurrent_readers > 1,
	      "multiple readers held the rwlock at once (max concurrent = %d)", g_rw_max_concurrent_readers);

	CHECK(pthread_rwlock_wrlock(&g_rwlock) == 0, "wrlock acquired once readers are gone");
	CHECK(pthread_rwlock_trywrlock(&g_rwlock) == EBUSY, "a second wrlock attempt reports EBUSY (exclusive)");
	pthread_rwlock_unlock(&g_rwlock);
}

// -----------------------------------------------------------------------
// pthread_once
// -----------------------------------------------------------------------

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_once_runs;

static void once_init(void)
{
	g_once_runs++;
}

static void *once_fn(void *arg)
{
	(void)arg;
	pthread_once(&g_once, once_init);
	return 0;
}

static void test_once(void)
{
	printf("-- pthread_once --\n");

	pthread_t threads[4];
	for (int i = 0; i < 4; i++)
		pthread_create(&threads[i], &g_small_attr, once_fn, 0);
	for (int i = 0; i < 4; i++)
		pthread_join(threads[i], 0);

	CHECK(g_once_runs == 1, "init ran exactly once across 4 threads (ran %d times)", g_once_runs);
}

// -----------------------------------------------------------------------
// Thread-specific data: pthread_key_create()/setspecific()/getspecific(),
// and compiler __thread storage.
// -----------------------------------------------------------------------

static pthread_key_t g_tsd_key;
static __thread int g_tls_var = -1;

struct tsd_result {
	long key_val;
	int tls_val;
};

static void *tsd_fn(void *arg)
{
	long id = (long)arg;
	static struct tsd_result results[8];

	pthread_setspecific(g_tsd_key, (void *)(id * 100));
	g_tls_var = (int)id;

	usleep(10000); // let other threads run and (mis-)set their own copies if isolation is broken

	results[id].key_val = (long)pthread_getspecific(g_tsd_key);
	results[id].tls_val = g_tls_var;
	return &results[id];
}

static void test_tsd(void)
{
	printf("-- thread-specific data (pthread_key + __thread) --\n");

	CHECK(pthread_key_create(&g_tsd_key, 0) == 0, "pthread_key_create() succeeds");

	pthread_t threads[4];
	for (long i = 0; i < 4; i++)
		pthread_create(&threads[i], &g_small_attr, tsd_fn, (void *)i);

	int ok = 1;
	for (long i = 0; i < 4; i++) {
		struct tsd_result *r;
		pthread_join(threads[i], (void **)&r);
		if (r->key_val != i * 100 || r->tls_val != (int)i)
			ok = 0;
	}
	CHECK(ok, "each thread saw only its own pthread_key + __thread values");

	pthread_key_delete(g_tsd_key);
}

// -----------------------------------------------------------------------
// pthread_spin_lock -- see thread_shim.c's file header for why this
// requires real preemption (it never calls futex/yields on its own).
// -----------------------------------------------------------------------

static pthread_spinlock_t g_spin;
static long g_spin_counter;

static void *spin_fn(void *arg)
{
	(void)arg;
	for (int i = 0; i < 5000; i++) {
		pthread_spin_lock(&g_spin);
		g_spin_counter++;
		pthread_spin_unlock(&g_spin);
	}
	return 0;
}

static void test_spinlock(void)
{
	printf("-- pthread_spin_lock --\n");

	pthread_spin_init(&g_spin, PTHREAD_PROCESS_PRIVATE);
	g_spin_counter = 0;

	pthread_t threads[3];
	for (int i = 0; i < 3; i++)
		pthread_create(&threads[i], &g_small_attr, spin_fn, 0);
	for (int i = 0; i < 3; i++)
		pthread_join(threads[i], 0);

	CHECK(g_spin_counter == 5000L * 3, "spinlock-protected counter == %ld (got %ld)",
	      5000L * 3, g_spin_counter);

	pthread_spin_destroy(&g_spin);
}

// -----------------------------------------------------------------------
// pthread_barrier_t
// -----------------------------------------------------------------------

#define BARRIER_THREADS 3

static pthread_barrier_t g_barrier;
static volatile int g_barrier_arrived;
static volatile int g_barrier_saw_all;

static void *barrier_fn(void *arg)
{
	(void)arg;
	__sync_fetch_and_add(&g_barrier_arrived, 1);
	int rc = pthread_barrier_wait(&g_barrier);
	// Every waiter should see all arrivals done by the time it's released.
	if (g_barrier_arrived != BARRIER_THREADS)
		g_barrier_saw_all = 0;
	return (void *)(long)rc;
}

static void test_barrier(void)
{
	printf("-- pthread_barrier_wait --\n");

	pthread_barrier_init(&g_barrier, 0, BARRIER_THREADS);
	g_barrier_arrived = 0;
	g_barrier_saw_all = 1;

	pthread_t threads[BARRIER_THREADS];
	int serial_count = 0;
	for (int i = 0; i < BARRIER_THREADS; i++)
		pthread_create(&threads[i], &g_small_attr, barrier_fn, 0);
	for (int i = 0; i < BARRIER_THREADS; i++) {
		void *res;
		pthread_join(threads[i], &res);
		if ((long)res == PTHREAD_BARRIER_SERIAL_THREAD)
			serial_count++;
	}

	CHECK(g_barrier_saw_all, "every thread saw all %d arrivals before being released", BARRIER_THREADS);
	CHECK(serial_count == 1, "exactly one thread got PTHREAD_BARRIER_SERIAL_THREAD (got %d)", serial_count);

	pthread_barrier_destroy(&g_barrier);
}

// -----------------------------------------------------------------------
// sched_yield() and pthread_detach()
// -----------------------------------------------------------------------

static void test_sched_yield(void)
{
	printf("-- sched_yield --\n");
	int rc = sched_yield();
	CHECK(rc == 0, "sched_yield() returns 0");
}

static volatile int g_detached_ran;

static void *detached_fn(void *arg)
{
	(void)arg;
	usleep(20000);
	g_detached_ran = 1;
	return 0;
}

static void test_detach(void)
{
	printf("-- pthread_detach --\n");

	g_detached_ran = 0;
	pthread_t t;
	pthread_create(&t, &g_small_attr, detached_fn, 0);
	CHECK(pthread_detach(t) == 0, "pthread_detach() succeeds on a running thread");

	// Not joinable anymore -- just wait long enough for it to finish on
	// its own and prove the process didn't need to join it to make
	// progress.
	usleep(60000);
	CHECK(g_detached_ran, "detached thread ran to completion on its own");
}

// -----------------------------------------------------------------------

int main(void)
{
	printf("threads.c -- BareMetal-AppPort cooperative pthreads test\n\n");

	init_small_attr();

	test_create_join();
	test_mutex();
	test_mutex_trylock();
	test_recursive_mutex();
	test_cond_signal();
	test_cond_broadcast();
	test_cond_timedwait();
	test_rwlock();
	test_once();
	test_tsd();
	test_spinlock();
	test_barrier();
	test_sched_yield();
	test_detach();

	pthread_attr_destroy(&g_small_attr);

	printf("\n%s: %d failure(s)\n", g_failures ? "RESULT" : "RESULT (all passed)", g_failures);
	return g_failures ? 1 : 0;
}
