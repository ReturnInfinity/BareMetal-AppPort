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

#endif

// =============================================================================
// EOF
