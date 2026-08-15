// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// thread_shim.h -- interface posix_shim.c's dispatcher uses to reach the
// cooperative thread scheduler (thread_shim.c). See that file's header
// for the design.
// =============================================================================

#ifndef _THREAD_SHIM_H
#define _THREAD_SHIM_H

// SYS_clone. `func` is smuggled in through the syscall's otherwise-unused
// 6th argument -- see thread_shim.c's file header for why musl's __clone
// puts it there.
long thread_shim_clone(long flags, long child_stack, long ptid, long ctid, long tls, long func);

// SYS_futex. Only FUTEX_WAIT/FUTEX_WAKE/FUTEX_REQUEUE (the ops musl's
// mutex/cond/join/barrier/once code actually issues) are implemented;
// anything else (the PI-futex ops, FUTEX_WAIT_BITSET, ...) is -ENOSYS.
long thread_shim_futex(long uaddr, long op, long val, long timeout_or_val2, long uaddr2, long val3);

// SYS_sched_yield.
long thread_shim_sched_yield(void);

// True once the calling context is a worker thread (created via
// thread_shim_clone()) rather than the app's original entry context.
// posix_shim.c's SYS_exit case uses this to decide whether a bare
// exit() call should tear down just this thread or the whole process --
// see its call site for why that distinction matters.
int thread_shim_is_worker_thread(void);

// Terminates the calling worker thread and switches to whatever should
// run next; never returns. Must only be called when
// thread_shim_is_worker_thread() is true.
void thread_shim_exit_current(void) __attribute__((noreturn));

// Real per-thread tid once threading is active (1 for the original
// thread otherwise) -- SYS_set_tid_address reports this.
long thread_shim_current_tid(void);

// -----------------------------------------------------------------------
// Signals -- per-thread pending/blocked sets plus a process-wide handler
// table, delivered from the same CALLBACK_TIMER injection point that
// already drives preemption. See thread_shim.c's "Signals" section for
// the full design and its honest limits (tick-granularity delivery, no
// hardware-fault-derived signals, no real ucontext_t).
// -----------------------------------------------------------------------

// SYS_rt_sigaction. `act`/`oldact` point at the kernel `struct k_sigaction`
// layout musl's __libc_sigaction() marshals into (handler, flags,
// restorer, mask[2]) -- not musl's app-facing `struct sigaction`.
// `sigsetsize` must be 8 (the only value musl ever passes on this arch).
long thread_shim_rt_sigaction(long sig, long act, long oldact, long sigsetsize);

// SYS_rt_sigprocmask. Operates on the calling thread's own blocked set
// (`set`/`oldset` point at a raw 64-bit mask, one bit per signal 1-64,
// matching the kernel `sigsetsize=8` ABI -- again not musl's 128-byte
// app-facing sigset_t).
long thread_shim_rt_sigprocmask(long how, long set, long oldset, long sigsetsize);

// SYS_tkill / SYS_tgkill (tgid ignored -- there is exactly one thread
// group, ever). Marks `sig` pending on the thread with the given real
// tid (thread_shim_current_tid()'s value), waking it if it's parked.
long thread_shim_tkill(long tid, long sig);

// SYS_kill. This port has exactly one process, so `pid` is not
// meaningful as a target selector -- signals the calling thread, the
// same thread raise()/pthread_kill(pthread_self(),...) would reach.
long thread_shim_kill(long pid, long sig);

// One-shot: true if a signal handler without SA_RESTART fired for the
// calling thread since the last call. posix_shim.c's/net_shim.c's own
// blocking loops (sleep_until_ns(), net_shim_recv() and friends) poll
// this each iteration to return -EINTR instead of finishing their wait,
// same as a real blocking syscall interrupted by a caught signal.
int thread_shim_take_eintr(void);

// Provided by posix_shim.c, not thread_shim.c: terminates the whole
// process via the same path SYS_exit_group does. thread_shim.c's signal
// delivery point (the timer tick) calls this for a signal whose
// disposition is "terminate" (SIG_DFL on a signal that isn't
// default-ignored, or SIGKILL, which cannot be caught or blocked).
void thread_shim_terminate_process(long code) __attribute__((noreturn));

#endif

// =============================================================================
// EOF
