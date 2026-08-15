// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// thread_shim.c -- cooperative user-level threads for musl's pthread_*
// API, backed entirely by userspace (ring-0-space) bookkeeping. There is
// no kernel thread list on this port (see OPENISSUES.md's old "Single-
// threaded only" note this file replaces) -- BareMetal `call`s an app
// once into one flat address space with no syscall trap, so real
// clone(2) (which duplicates a task's execution at the kernel level and
// resumes it on another core) has nothing to hook into.
//
// Instead, SYS_clone here builds a second call stack by hand and the
// timer tick fed to b_system(CALLBACK_TIMER, ...) -- see below -- swaps
// between them in round robin, exactly the way a single-core preemptive
// scheduler would. "Cooperative" describes what these threads are, not
// how the switching happens: there's no SMP, no priorities, and no
// separate address spaces to isolate -- every thread already shares
// everything (memory, fds, the EXT2/socket tables) because that's just
// what "one process, one flat address space" already means here, not
// something CLONE_VM had to arrange. What *is* real is the concurrency:
// threads interleave at every timer tick (1ms, see init_sys in the
// kernel), including inside another thread's blocking read()/recv()/
// nanosleep() spin-wait, so a worker sitting in a blocking call doesn't
// starve everyone else the way it would with a purely yield-point-only
// (e.g. no CALLBACK_TIMER at all) scheduler.
//
// == How b_system(CALLBACK_TIMER, ...) makes this possible ==
//
// The registered callback is not an ordinary interrupt handler. Per
// BareMetal-Firecracker's int_apic_timer (interrupt.asm), the kernel
// rewrites the interrupted code's own IRETQ frame so that -- after the
// timer IRQ is acknowledged -- control lands in the callback *as if it
// had been `call`ed from the interrupted instruction*, with every
// register (including RAX/RCX, saved/restored around the whole thing)
// exactly as the app left them. When the callback eventually `ret`s,
// execution resumes exactly where the interrupt found it. That is: the
// kernel hands us a genuine, hardware-timed "insert a function call
// here" hook into whatever the app is doing, on the app's own stack.
//
// That is also exactly what a hand-rolled coroutine switch needs: since
// bmos_ctx_switch() below is itself just an ordinary call/ret-disciplined
// function, nesting one inside the fabricated call changes nothing about
// how either mechanism works. Suspending a thread here means saving the
// callee-saved registers (the only ones a `call` boundary obligates
// anyone to preserve) and swapping RSP; resuming it later means loading
// its RSP back and `ret`-ing into wherever it last called out from --
// which, transitively, is however many stack frames up to and including
// that fabricated timer call, and from there straight back into the
// preempted app code. No arbitrary-signal-handler tricks, no saved FPU/
// SSE state, no second stack per suspend point -- just call/ret, twice
// over.
//
// == Why real preemption is safe without a "cooperative-only" design ==
//
// musl's own concurrency primitives (mallocng's heap lock, a_cas-based
// mutexes, etc.) are built to be safe under real preemption already --
// that's the whole point of a lock built on a single atomic instruction
// plus a futex fallback. The only piece this port has to supply itself
// is that futex fallback (FUTEX_WAIT/FUTEX_WAKE/FUTEX_REQUEUE below);
// the atomic instructions (a_cas et al, x86_64 asm, untouched by this
// port) are already indivisible with respect to an interrupt on a single
// core. So plain preemptive round-robin is not just easier than
// instrumenting safe points everywhere -- it's the only design under
// which something like pthread_spin_lock() (a bare a_cas retry loop,
// see musl's pthread_spin_lock.c -- no futex, no voluntary yield at
// all) can ever unblock: with switching restricted to explicit yield
// points, a thread spinning on a lock held by a thread that never yields
// would spin forever.
// =============================================================================

#define _GNU_SOURCE
#include <sched.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <ucontext.h>

#include "libBareMetal.h"
#include "thread_shim.h"

#define THREAD_SHIM_MAX_THREADS	32
#define THREAD_SHIM_IDLE_POLL_NS	1000000ULL	// 1ms -- matches the kernel's own tick rate

enum thread_state {
	T_FREE = 0,	// slot unused (also zero_bss()'s initial state)
	T_READY,	// on the ready queue, not running
	T_RUNNING,	// this is bmos_current
	T_BLOCKED,	// parked on a futex address or a timeout, not queued
};

enum wake_reason {
	WOKEN_NONE = 0,
	WOKEN_SIGNAL,	// FUTEX_WAKE -- unrelated to the POSIX signals below
	WOKEN_TIMEOUT,
	WOKEN_INTR,	// a deliverable POSIX signal arrived while parked
};

struct bmos_thread {
	unsigned long rsp;		// saved stack pointer -- meaningless while RUNNING
	unsigned long tls;		// FS base to install while this thread runs
	int tid;
	enum thread_state state;
	int (*entry)(void *);		// musl's start()/start_c11() (see pthread_create.c)
	void *arg;
	int *ctid;			// CLONE_CHILD_CLEARTID target, or NULL
	volatile int *wait_addr;	// futex address, valid while T_BLOCKED
	u64 wait_deadline_ns;		// TIMECOUNTER deadline, 0 = no timeout
	enum wake_reason wake_reason;
	struct bmos_thread *next;	// ready-queue link

	// Signals -- see this file's "Signals" section. One bit per signal
	// (1-64, bit sig-1), matching the kernel rt_sigprocmask/rt_sigaction
	// ABI's fixed 8-byte mask, not musl's 128-byte app-facing sigset_t.
	unsigned long sig_pending;
	unsigned long sig_blocked;
	int sig_eintr;			// one-shot flag, see thread_shim_take_eintr()

	// FXSAVE/FXRSTOR image (x87/MMX/SSE state) -- bmos_ctx_switch()
	// only ever saves/restores the general-purpose registers a `call`
	// boundary obligates a callee to preserve (see its own comment).
	// XMM0-15 are all caller-saved in the SysV ABI, so ordinary
	// compiled code (this port's own C, and musl's, some of it built
	// at -O2 and free to use SSE for e.g. memcpy/string ops) never
	// spills them around a call it makes itself -- but the timer tick
	// this scheduler switches on (see this file's header) can
	// interrupt *any* instruction, not just call boundaries, so it can
	// catch a thread with XMM registers legitimately live for its next
	// instruction. Without this, switching threads there clobbers
	// them with whatever the next thread(s) to run happen to leave
	// behind -- silent data corruption in whichever computation was
	// mid-flight (observed as garbled printf() output -- musl's
	// vfprintf/memcpy at -O2 uses SSE). 16-byte-aligned: FXSAVE/FXRSTOR
	// fault (#GP) on a misaligned operand.
	unsigned char fxarea[512] __attribute__((aligned(16)));
};

static struct bmos_thread g_main_thread;
static struct bmos_thread g_threads[THREAD_SHIM_MAX_THREADS];
static struct bmos_thread *g_current;
static struct bmos_thread *g_ready_head, *g_ready_tail;
static int g_next_tid = 2;	// 1 is the original (main) thread
static int g_active;

static void thread_shim_timer_tick(void);

static inline void irq_disable(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void irq_enable(void)  { __asm__ volatile ("sti" ::: "memory"); }

static inline void fxsave_state(unsigned char *area)
{
	__asm__ volatile ("fxsave (%0)" :: "r"(area) : "memory");
}

static inline void fxrstor_state(unsigned char *area)
{
	__asm__ volatile ("fxrstor (%0)" :: "r"(area) : "memory");
}

static inline unsigned long get_fsbase(void)
{
	unsigned lo, hi;
	__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
	return ((unsigned long)hi << 32) | lo;
}

static inline void set_fsbase(unsigned long v)
{
	unsigned lo = (unsigned)v, hi = (unsigned)(v >> 32);
	__asm__ volatile ("wrmsr" :: "c"(0xC0000100), "a"(lo), "d"(hi));
}

// -----------------------------------------------------------------------
// Context switch primitives
//
// Both are naked: there is no C-visible body, only the exact sequence of
// pushes/pops a resuming thread expects to find. A fresh thread (built
// by thread_shim_clone(), below) never made the call that got it here,
// so its initial stack is hand-laid-out to look exactly like a thread
// that's mid-way through bmos_ctx_switch(), about to pop 6 register
// slots and `ret` into bmos_thread_trampoline(). See crt0.c's _start()
// for the same naked-function technique used the same way.
// -----------------------------------------------------------------------

__attribute__((naked)) static void bmos_ctx_switch(unsigned long *save_rsp, unsigned long new_rsp)
{
	__asm__ volatile (
		"pushq %%rbp\n\t"
		"pushq %%rbx\n\t"
		"pushq %%r12\n\t"
		"pushq %%r13\n\t"
		"pushq %%r14\n\t"
		"pushq %%r15\n\t"
		"movq %%rsp, (%%rdi)\n\t"
		"movq %%rsi, %%rsp\n\t"
		"popq %%r15\n\t"
		"popq %%r14\n\t"
		"popq %%r13\n\t"
		"popq %%r12\n\t"
		"popq %%rbx\n\t"
		"popq %%rbp\n\t"
		"ret\n\t"
		::: "memory"
	);
}

// Same tail half as bmos_ctx_switch(), without the save -- used only by
// thread_shim_exit_current(), which has no "self" worth resuming later.
__attribute__((naked, noreturn)) static void bmos_ctx_enter(unsigned long new_rsp)
{
	__asm__ volatile (
		"movq %%rdi, %%rsp\n\t"
		"popq %%r15\n\t"
		"popq %%r14\n\t"
		"popq %%r13\n\t"
		"popq %%r12\n\t"
		"popq %%rbx\n\t"
		"popq %%rbp\n\t"
		"ret\n\t"
		::: "memory"
	);
}

static void bmos_thread_trampoline(void);

// -----------------------------------------------------------------------
// Ready queue + scheduler
// -----------------------------------------------------------------------

static void enqueue_ready_locked(struct bmos_thread *t)
{
	t->next = 0;
	if (g_ready_tail)
		g_ready_tail->next = t;
	else
		g_ready_head = t;
	g_ready_tail = t;
}

static struct bmos_thread *dequeue_ready_locked(void)
{
	struct bmos_thread *t = g_ready_head;
	if (t) {
		g_ready_head = t->next;
		if (!g_ready_head)
			g_ready_tail = 0;
	}
	return t;
}

static void wake_thread_locked(struct bmos_thread *t, enum wake_reason reason)
{
	t->wait_addr = 0;
	t->wake_reason = reason;
	t->state = T_READY;
	enqueue_ready_locked(t);
}

// g_main_thread (the original, non-clone()d execution context) is a
// separate static, not a slot in g_threads[] -- alloc_thread_slot() only
// ever hands out g_threads[] entries to thread_shim_clone(), and that's
// the only thing that should be true of it. But it can still block on a
// futex/timeout exactly like any worker (see thread_shim_futex()), so
// anything scanning "every thread that might be parked" -- futex_wake()
// and scan_timeouts() below -- has to visit it too, one more iteration
// than THREAD_SHIM_MAX_THREADS.
static struct bmos_thread *thread_at(int i)
{
	return i == 0 ? &g_main_thread : &g_threads[i - 1];
}
#define THREAD_SHIM_ALL_THREADS (THREAD_SHIM_MAX_THREADS + 1)

static struct bmos_thread *find_thread_by_tid(int tid)
{
	// Before thread_shim_init() ever runs (no pthread_create() yet),
	// g_main_thread.state is still T_FREE (BSS zero-init) even though
	// it's the one and only real thread -- same "tid 1" convention
	// thread_shim_current_tid() already uses for this exact window.
	if (!g_active)
		return tid == 1 ? &g_main_thread : 0;

	for (int i = 0; i < THREAD_SHIM_ALL_THREADS; i++) {
		struct bmos_thread *t = thread_at(i);
		if (t->state != T_FREE && t->tid == tid)
			return t;
	}
	return 0;
}

// Promotes any T_BLOCKED thread whose timeout has passed, or that now
// has a deliverable (pending and not blocked) signal, to T_READY.
// Called from the timer tick (so a sleeping FUTEX_WAIT/timedjoin/
// timedwait/thread_shim_tkill() target eventually gets a chance to run
// even if nobody explicitly wakes it) and from the idle-poll loops below
// (so the same happens even while every thread, including the one
// driving that loop, is blocked). The signal itself is *not* consumed
// here -- see thread_shim_timer_tick_c()'s deliver_pending_signals(),
// which runs the actual handler once this thread is running again.
static void scan_timeouts(void)
{
	u64 now = 0;
	int have_now = 0;

	irq_disable();
	for (int i = 0; i < THREAD_SHIM_ALL_THREADS; i++) {
		struct bmos_thread *t = thread_at(i);
		if (t->state != T_BLOCKED)
			continue;
		if (t->sig_pending & ~t->sig_blocked) {
			wake_thread_locked(t, WOKEN_INTR);
			continue;
		}
		if (t->wait_deadline_ns) {
			if (!have_now) {
				now = b_system(TIMECOUNTER, 0, 0);
				have_now = 1;
			}
			if (now >= t->wait_deadline_ns)
				wake_thread_locked(t, WOKEN_TIMEOUT);
		}
	}
	irq_enable();
}

// Switches away from bmos_current. If `requeue` is set, the current
// thread goes back on the ready queue first (a plain preemption/
// sched_yield()); otherwise the caller has already parked it (T_BLOCKED,
// with wait_addr/wait_deadline_ns set) or is exiting, and it is left off
// every queue -- only an explicit wake (wake_thread_locked(), via
// futex_wake/scan_timeouts) can make it runnable again.
//
// Returns once this thread is resumed (irrelevant for a thread that's
// exiting -- see thread_shim_exit_current(), which uses bmos_ctx_enter()
// instead precisely because it never needs to come back).
static void reschedule(int requeue)
{
	struct bmos_thread *self = g_current;

	for (;;) {
		irq_disable();

		if (requeue) {
			self->state = T_READY;
			enqueue_ready_locked(self);
		}

		struct bmos_thread *next = dequeue_ready_locked();

		if (next == self) {
			// We were the only ready thread -- nothing to switch to.
			self->state = T_RUNNING;
			irq_enable();
			return;
		}

		if (next) {
			next->state = T_RUNNING;
			g_current = next;
			set_fsbase(next->tls);
			fxsave_state(self->fxarea);
			fxrstor_state(next->fxarea);
			bmos_ctx_switch(&self->rsp, next->rsp);
			// Resumed (possibly much later, possibly by a
			// different thread's reschedule() than the one that
			// switched us out): interrupts were disabled by
			// whichever call site put us here, so re-enable
			// before returning to our own caller. FS base and the
			// FPU/SSE state are already exactly as we left them --
			// whoever switched *into* us did that half themselves,
			// symmetrically, on their way out (see above).
			irq_enable();
			return;
		}

		// Nothing runnable at all right now. If we're only yielding
		// (requeue) this can't happen -- self was just enqueued --
		// so this is the "parked, nobody else to run" case: idle
		// until scan_timeouts() (run right here, and from the timer
		// tick) promotes someone -- possibly self -- back to ready.
		irq_enable();
		if (!requeue) {
			scan_timeouts();
			b_system(SLEEP, THREAD_SHIM_IDLE_POLL_NS, 0);
			continue;
		}
		return;
	}
}

// -----------------------------------------------------------------------
// Signals
//
// Real POSIX signal delivery needs the kernel to rewrite an interrupted
// hardware frame to detour into a handler and later sigreturn -- this
// port has no such hook in general, but it has exactly one, for exactly
// this purpose: b_system(CALLBACK_TIMER, ...) already inserts a
// fabricated *call* at whatever instruction it interrupted (see this
// file's header), on every thread, roughly every 1ms. That's the same
// injection point thread_shim_timer_tick_c() already uses to preempt --
// reused here to check the interrupted (i.e. currently running) thread's
// pending signals and, if one is deliverable, call its handler directly
// as an ordinary C function from right here. No hardware signal frame,
// no rt_sigreturn: the handler just runs to completion and this
// function's caller resumes normally afterward, exactly like any other
// nested call reschedule() already makes safe to run from this same
// call site (including a full context switch to a different thread and
// back).
//
// Honest limits:
//   - Delivery latency is bounded by the timer tick (~1ms), not
//     instantaneous -- fine for pthread_kill()/raise()-style use, not
//     for anything expecting sub-tick real-time semantics.
//   - No hardware-fault-derived signals (a real SIGSEGV/SIGFPE/SIGBUS
//     from an actual trap) -- this only covers software-raised signals
//     (kill/tkill/tgkill/raise). Translating a real BareMetal-kernel
//     fault into a signal would need kernel-side work, out of scope
//     here.
//   - SA_SIGINFO handlers get a real siginfo_t/ucontext_t, but with an
//     all-zero mcontext (no meaningful uc_mcontext.MC_PC/gregs) -- there
//     is no real interrupted-hardware-frame to expose the way a genuine
//     signal trampoline would. musl's own SIGCANCEL handler
//     (pthread_cancel.c's cancel_handler()) copes with this fine: its
//     PC-in-cancellation-point-range check just always misses (falling
//     through to its self-tkill "keep pending" path), and cross-thread
//     pthread_cancel() actually takes effect via the EINTR path below
//     instead -- see futex_wait()'s WOKEN_INTR case and
//     __syscall_cp_asm's pre-call cancel check (musl-1.2.6/src/thread/
//     x86_64/syscall_cp.s), which needs nothing extra from here.
// -----------------------------------------------------------------------

// Raw kernel rt_sigaction ABI (src/internal/ksigaction.h) -- what
// musl's __libc_sigaction() actually marshals `act`/`oldact` into on
// this arch, distinct from (and much smaller than) the app-facing
// `struct sigaction`/128-byte sigset_t this file deliberately avoids
// depending on.
struct bmos_ksigaction {
	void (*handler)(int);
	unsigned long flags;
	void (*restorer)(void);
	unsigned mask[2];
};

// Process-wide -- there is exactly one process, ever, and real POSIX
// signal dispositions are process-wide (shared across threads) too, so
// this is not itself a per-thread thing the way pending/blocked are.
// Index sig-1, signals 1-64 (_NSIG == 65 on this arch). handler encodes
// SIG_DFL (0) / SIG_IGN (1) / a real function pointer, same as musl's
// own k_sigaction.handler field.
struct bmos_sigaction {
	unsigned long handler;
	unsigned long flags;
	unsigned long mask;
};
static struct bmos_sigaction g_sigact[64];

// Default (SIG_DFL) disposition for signals this port has any use for.
// Matches real Linux defaults except job-control signals (SIGSTOP et
// al), which are treated as ignored rather than actually stopping
// anything -- there is no job control (no shell, no process group) on
// this port for a stop to mean anything to. Everything else not listed
// (SIGHUP/SIGINT/SIGQUIT/SIGABRT/SIGSEGV/SIGTERM/the realtime range/...)
// defaults to terminating the process, same as real Linux.
static int default_is_ignore(int sig)
{
	switch (sig) {
	case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT:
	case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
		return 1;
	default:
		return 0;
	}
}

// Delivers at most one pending, unblocked signal to `t`, which must be
// the currently running thread (only a running thread can safely have a
// handler called on its behalf -- see this section's header). Called
// from thread_shim_timer_tick_c() below.
static void deliver_pending_signals(struct bmos_thread *t)
{
	unsigned long deliverable = t->sig_pending & ~t->sig_blocked;
	if (!deliverable)
		return;

	// Lowest-numbered pending signal first, same tie-break real Linux
	// uses when more than one is deliverable at once.
	int sig = __builtin_ctzl(deliverable) + 1;

	irq_disable();
	t->sig_pending &= ~(1UL << (sig - 1));
	irq_enable();

	// SIGKILL can't be caught, blocked, or ignored -- terminate
	// unconditionally regardless of g_sigact.
	if (sig == SIGKILL)
		thread_shim_terminate_process(128 + SIGKILL);

	struct bmos_sigaction *sa = &g_sigact[sig - 1];

	if (sa->handler == 1) // SIG_IGN
		return;
	if (sa->handler == 0) { // SIG_DFL
		if (default_is_ignore(sig))
			return;
		thread_shim_terminate_process(128 + sig);
	}

	// A real handler installed -- block sa_mask (plus this signal
	// itself, unless SA_NODEFER) for the duration, same as a real
	// kernel would, and restore afterward.
	unsigned long saved_blocked = t->sig_blocked;
	t->sig_blocked |= sa->mask;
	if (!(sa->flags & SA_NODEFER))
		t->sig_blocked |= (1UL << (sig - 1));

	// See sleep_until_ns()/net_shim.c's blocking loops (posix_shim.c/
	// net_shim.c) -- SA_RESTART means "the interrupted blocking call
	// should keep waiting", so this stays 0 for exactly the handlers
	// that should surface as a real EINTR to the caller.
	if (!(sa->flags & SA_RESTART))
		t->sig_eintr = 1;

	if (sa->flags & SA_SIGINFO) {
		siginfo_t si;
		ucontext_t uc;
		__builtin_memset(&si, 0, sizeof si);
		__builtin_memset(&uc, 0, sizeof uc); // see this section's header on uc_mcontext
		si.si_signo = sig;
		uc.uc_sigmask.__bits[0] = t->sig_blocked;
		void (*fn)(int, siginfo_t *, void *) =
			(void (*)(int, siginfo_t *, void *))(unsigned long)sa->handler;
		fn(sig, &si, &uc);
	} else {
		void (*fn)(int) = (void (*)(int))(unsigned long)sa->handler;
		fn(sig);
	}

	if (sa->flags & SA_RESETHAND) {
		sa->handler = 0; // SIG_DFL
		sa->flags = 0;
	}

	t->sig_blocked = saved_blocked;
}

// The timer tick registered with b_system(CALLBACK_TIMER, ...) -- see
// this file's header. Only preempts a thread that's actually running
// (never a thread another reschedule() call already parked as
// T_BLOCKED on this same stack, e.g. the idle-poll loop above).
static void thread_shim_timer_tick_c(void)
{
	if (!g_active)
		return;
	scan_timeouts();
	if (g_current)
		deliver_pending_signals(g_current);
	if (g_current && g_current->state == T_RUNNING && g_ready_head)
		reschedule(1);
}

// b_system(CALLBACK_TIMER, ...) inserts this as a fabricated *call* at
// the interrupted code's current instruction (see this file's header) --
// it is not a real ISR, and critically, unlike a real one it does NOT by
// itself preserve the interrupted code's caller-saved registers or
// RFLAGS across the call: the kernel trampoline (interrupt.asm) only
// restores RAX/RCX to their pre-interrupt values *before* invoking this,
// not after. An ordinary compiled C function is free to clobber every
// caller-saved GPR and any flag per the SysV ABI, which is exactly what
// thread_shim_timer_tick_c() (and anything it calls, including a full
// thread switch) does. That's invisible for ordinary call-boundary code
// (the compiler already spills anything live across a real call) but
// not for an instruction the interrupt can land on *without* one ever
// existing in the source -- most importantly a `rep movsb/stosb`
// (musl's -O2 memcpy/memset/etc.) where RSI/RDI/RCX/RFLAGS(DF) are live
// *state of that single instruction*, not call-clobberable temporaries;
// clobbering them and resuming corrupts whatever the copy/fill touches
// next with no fault to show for it. So this thin naked wrapper is the
// actual registered callback: it saves every caller-saved GPR and
// RFLAGS before doing anything else, calls the real C logic (free to
// clobber all of that, including via a full reschedule() switch away
// and eventually back -- these pushes just sit on this thread's own
// stack, preserved the same way everything else about a suspended
// thread is), and restores them right before the `ret` that hands
// control back to the interrupted instruction.
__attribute__((naked)) static void thread_shim_timer_tick(void)
{
	__asm__ volatile (
		"pushfq\n\t"
		"pushq %%rax\n\t"
		"pushq %%rcx\n\t"
		"pushq %%rdx\n\t"
		"pushq %%rsi\n\t"
		"pushq %%rdi\n\t"
		"pushq %%r8\n\t"
		"pushq %%r9\n\t"
		"pushq %%r10\n\t"
		"pushq %%r11\n\t"
		"call thread_shim_timer_tick_c\n\t"
		"popq %%r11\n\t"
		"popq %%r10\n\t"
		"popq %%r9\n\t"
		"popq %%r8\n\t"
		"popq %%rdi\n\t"
		"popq %%rsi\n\t"
		"popq %%rdx\n\t"
		"popq %%rcx\n\t"
		"popq %%rax\n\t"
		"popfq\n\t"
		"ret\n\t"
		::: "memory"
	);
}

static void thread_shim_init(void)
{
	if (g_active)
		return;

	g_main_thread.tid = 1;
	g_main_thread.state = T_RUNNING;
	g_main_thread.tls = get_fsbase();
	g_current = &g_main_thread;
	g_active = 1;

	b_system(CALLBACK_TIMER, (u64)(unsigned long)thread_shim_timer_tick, 0);
}

static struct bmos_thread *alloc_thread_slot(void)
{
	for (int i = 0; i < THREAD_SHIM_MAX_THREADS; i++) {
		if (g_threads[i].state == T_FREE)
			return &g_threads[i];
	}
	return 0;
}

// -----------------------------------------------------------------------
// SYS_clone
//
// musl's __clone (src/thread/x86_64/clone.s) is a *library* wrapper, not
// the raw kernel entry point: it issues the real 5-argument clone(2)
// syscall (flags, stack, ptid, ctid, tls -- rdi/rsi/rdx/r10/r8) and then,
// in the child only (kernel-level clone() returns twice, like fork()),
// pops `args` off the new stack and calls the thread's real entry
// function through it -- smuggled across the fork in r9, a register the
// raw clone(2) syscall itself never uses. There is no second return
// here (nothing forks control flow inside a plain function call), so
// this builds that child context by hand instead: the stack musl already
// carved out (child_stack, with `args` sitting at its top -- exactly
// what __clone's child path would have `pop`ped) becomes a fresh
// bmos_ctx_switch()-shaped frame that resumes in bmos_thread_trampoline()
// the first time anything switches to it.
// -----------------------------------------------------------------------

long thread_shim_clone(long flags, long child_stack, long ptid, long ctid, long tls, long func)
{
	if (!(flags & CLONE_VM))
		return -ENOSYS; // only the pthread_create() shape is supported

	thread_shim_init();

	struct bmos_thread *t = alloc_thread_slot();
	if (!t)
		return -EAGAIN;

	t->tid = g_next_tid++;
	t->tls = (unsigned long)tls;
	t->entry = (int (*)(void *))(unsigned long)func;
	t->arg = *(void **)(unsigned long)child_stack; // musl already pushed this
	t->ctid = (flags & CLONE_CHILD_CLEARTID) ? (int *)(unsigned long)ctid : 0;
	t->wait_addr = 0;
	t->wait_deadline_ns = 0;
	t->wake_reason = WOKEN_NONE;
	t->sig_pending = 0;			// pending signals are per-thread, not inherited
	t->sig_blocked = g_current->sig_blocked; // the mask itself is, same as real pthread_create()
	t->sig_eintr = 0;

	// Seed a legal FXSAVE image for this not-yet-run thread by
	// capturing the creating thread's own current (live, therefore
	// always valid) FPU/SSE state, rather than leaving fxarea zeroed
	// -- a zeroed image isn't guaranteed a legal FXRSTOR operand (e.g.
	// the control-word field), and this thread's fxarea gets an
	// unconditional FXRSTOR the first time anything switches to it
	// (see reschedule()'s and thread_shim_exit_current()'s comments).
	fxsave_state(t->fxarea);

	// Lay out a frame bmos_ctx_switch()'s epilogue can resume into: 6
	// register slots (values irrelevant -- bmos_thread_trampoline()
	// doesn't depend on incoming register state) followed by the
	// "return address" it'll `ret` into. child_stack arrives already
	// aligned the way a real `call bmos_ctx_switch` site would leave
	// it (see musl's __clone: `and $-16,%rsi; sub $8,%rsi`), so no
	// further alignment here -- just subtracting the 7 qwords below
	// preserves it.
	unsigned long *sp = (unsigned long *)(unsigned long)child_stack;
	sp -= 7;
	sp[0] = sp[1] = sp[2] = sp[3] = sp[4] = sp[5] = 0;
	sp[6] = (unsigned long)bmos_thread_trampoline;
	t->rsp = (unsigned long)sp;

	if (flags & CLONE_PARENT_SETTID)
		*(int *)(unsigned long)ptid = t->tid;

	irq_disable();
	t->state = T_READY;
	enqueue_ready_locked(t);
	irq_enable();

	return t->tid;
}

// First code a new thread ever runs, reached via bmos_ctx_switch()'s
// `ret` (see thread_shim_clone() above) rather than a normal call --
// nobody else's reschedule() call is waiting to re-enable interrupts
// for us on the way out, so that happens here instead, mirroring
// reschedule()'s own post-switch irq_enable().
static void bmos_thread_trampoline(void)
{
	irq_enable();

	struct bmos_thread *self = g_current;
	self->entry(self->arg);

	// musl's start()/start_c11() (pthread_create.c) always end in
	// __pthread_exit(), which never returns -- but guard anyway
	// rather than run off the end of this thread's stack.
	thread_shim_exit_current();
}

// -----------------------------------------------------------------------
// Thread exit -- see posix_shim.c's SYS_exit case for why this only
// runs for a bare exit() from a worker thread, never the whole process.
// -----------------------------------------------------------------------

int thread_shim_is_worker_thread(void)
{
	return g_active && g_current && g_current != &g_main_thread;
}

long thread_shim_current_tid(void)
{
	return g_active && g_current ? g_current->tid : 1;
}

static long futex_wake(volatile int *addr, int n);

void thread_shim_exit_current(void)
{
	irq_disable();

	struct bmos_thread *self = g_current;
	self->state = T_BLOCKED; // off every queue, same as any other parked thread below
	self->wait_addr = 0; // never wake an exiting thread back onto the ready queue
	int *ctid = self->ctid;

	irq_enable();

	// Mirrors CLONE_CHILD_CLEARTID's kernel-level behavior on real
	// Linux: clear *ctid and wake anyone parked on it. musl's own
	// __pthread_exit() already wakes joiners itself via
	// self->detach_state before ever reaching SYS_exit (see
	// pthread_create.c), so this mainly covers musl's redundant
	// safety-net use of it (ctid = &__thread_list_lock, guarding
	// against a thread dying before it calls __tl_unlock()). Safe to
	// do with interrupts back on: self is already parked (T_BLOCKED,
	// off every queue) so a tick landing here just runs scan_timeouts()
	// and returns -- see thread_shim_timer_tick()'s T_RUNNING guard.
	if (ctid) {
		*ctid = 0;
		futex_wake((volatile int *)ctid, 1);
	}

	irq_disable();
	struct bmos_thread *next = dequeue_ready_locked();
	while (!next) {
		irq_enable();
		scan_timeouts();
		b_system(SLEEP, THREAD_SHIM_IDLE_POLL_NS, 0);
		irq_disable();
		next = dequeue_ready_locked();
	}

	next->state = T_RUNNING;
	g_current = next;
	set_fsbase(next->tls);
	fxrstor_state(next->fxarea); // see reschedule()'s comment on struct bmos_thread's fxarea
	self->state = T_FREE; // recycle -- nothing of ours references this slot again
	bmos_ctx_enter(next->rsp);
	__builtin_unreachable();
}

// -----------------------------------------------------------------------
// SYS_sched_yield
// -----------------------------------------------------------------------

long thread_shim_sched_yield(void)
{
	if (!g_active)
		return 0; // nothing else could possibly be runnable
	reschedule(1);
	return 0;
}

// -----------------------------------------------------------------------
// SYS_futex
//
// Only the three ops musl's own thread code ever issues in this port's
// single-process, non-PI, non-robust-by-default configuration:
//   FUTEX_WAIT (0)     -- block while *uaddr == val
//   FUTEX_WAKE (1)     -- ready up to `val` threads blocked on uaddr
//   FUTEX_REQUEUE (3)  -- see futex_requeue()'s comment
// The private/realtime-clock flag bits (128, 256) are accepted and
// ignored: there is only one process, ever, so "private" is always true,
// and nothing here distinguishes a monotonic from a realtime deadline.
// -----------------------------------------------------------------------

#define FUTEX_OP_MASK	0x7f

static long futex_wait(volatile int *addr, int val, const struct timespec *timeout)
{
	if (!g_active)
		return -EAGAIN; // no other thread could ever exist to wake us
	if (*addr != val)
		return -EAGAIN;

	struct bmos_thread *self = g_current;

	// A signal already deliverable at entry never blocks at all, same
	// as a real kernel's FUTEX_WAIT -- don't wait a whole tick for
	// scan_timeouts() to notice what's already true right now.
	if (self->sig_pending & ~self->sig_blocked)
		return -EINTR;

	u64 deadline = 0;
	if (timeout) {
		u64 rel_ns = (u64)timeout->tv_sec * 1000000000ULL + (u64)timeout->tv_nsec;
		deadline = b_system(TIMECOUNTER, 0, 0) + rel_ns;
	}

	irq_disable();
	if (*addr != val) {
		irq_enable();
		return -EAGAIN;
	}
	self->state = T_BLOCKED;
	self->wait_addr = addr;
	self->wait_deadline_ns = deadline;
	self->wake_reason = WOKEN_NONE;
	irq_enable();

	reschedule(0);

	enum wake_reason why = self->wake_reason;
	self->wait_deadline_ns = 0;
	if (why == WOKEN_INTR)
		return -EINTR;
	return why == WOKEN_TIMEOUT ? -ETIMEDOUT : 0;
}

static long futex_wake(volatile int *addr, int n)
{
	if (!g_active || n <= 0)
		return 0;

	int woken = 0;
	irq_disable();
	for (int i = 0; i < THREAD_SHIM_ALL_THREADS && woken < n; i++) {
		struct bmos_thread *t = thread_at(i);
		if (t->state == T_BLOCKED && t->wait_addr == addr) {
			wake_thread_locked(t, WOKEN_SIGNAL);
			woken++;
		}
	}
	irq_enable();
	return woken;
}

// A real kernel FUTEX_REQUEUE wakes `nr_wake` waiters on uaddr1 and
// silently relocates up to `nr_requeue` more to block on uaddr2 instead
// (musl's only user, pthread_cond_timedwait.c's unlock_requeue(), uses
// it to hand a broadcast's waiters to the mutex one at a time without a
// thundering herd). This port has no real second wait queue to relocate
// anyone onto -- but every "requeued" waiter here is, by construction,
// about to call pthread_mutex_lock() itself the moment it's runnable
// again (that's what unlock_requeue()'s caller does right after), so
// simply waking all nr_wake + nr_requeue of them and letting them
// re-contend the mutex the ordinary way (via FUTEX_WAIT on the mutex
// word, ending up back in futex_wait() above) reaches the same end
// state -- just via one extra wake/re-block instead of a silent
// relocation.
static long futex_requeue(volatile int *addr, int nr_wake, int nr_requeue)
{
	return futex_wake(addr, nr_wake + nr_requeue);
}

long thread_shim_futex(long uaddr, long op, long val, long timeout_or_val2, long uaddr2, long val3)
{
	(void)uaddr2; (void)val3;

	volatile int *addr = (volatile int *)(unsigned long)uaddr;
	int cmd = (int)(op & FUTEX_OP_MASK);

	switch (cmd) {
	case 0: // FUTEX_WAIT
		return futex_wait(addr, (int)val, (const struct timespec *)(unsigned long)timeout_or_val2);
	case 1: // FUTEX_WAKE
		return futex_wake(addr, (int)val);
	case 3: // FUTEX_REQUEUE
		return futex_requeue(addr, (int)val, (int)timeout_or_val2);
	default:
		return -ENOSYS;
	}
}

// -----------------------------------------------------------------------
// SYS_rt_sigaction / SYS_rt_sigprocmask / SYS_tkill / SYS_tgkill / SYS_kill
//
// posix_shim.c's dispatcher forwards these straight here -- see this
// file's "Signals" section above for the delivery mechanism these feed.
// -----------------------------------------------------------------------

long thread_shim_rt_sigaction(long sig, long act, long oldact, long sigsetsize)
{
	if (sig < 1 || sig > 64 || sigsetsize != 8)
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP) // can never be caught/ignored
		return -EINVAL;

	struct bmos_sigaction *sa = &g_sigact[sig - 1];

	if (oldact) {
		struct bmos_ksigaction *o = (struct bmos_ksigaction *)(unsigned long)oldact;
		o->handler = (void (*)(int))(unsigned long)sa->handler;
		o->flags = sa->flags;
		o->mask[0] = (unsigned)sa->mask;
		o->mask[1] = (unsigned)(sa->mask >> 32);
	}
	if (act) {
		struct bmos_ksigaction *a = (struct bmos_ksigaction *)(unsigned long)act;
		sa->handler = (unsigned long)(void *)a->handler;
		sa->flags = a->flags;
		sa->mask = (unsigned long)a->mask[0] | ((unsigned long)a->mask[1] << 32);
	}
	return 0;
}

long thread_shim_rt_sigprocmask(long how, long set, long oldset, long sigsetsize)
{
	if (sigsetsize != 8)
		return -EINVAL;

	struct bmos_thread *self = (g_active && g_current) ? g_current : &g_main_thread;

	if (oldset)
		*(unsigned long *)(unsigned long)oldset = self->sig_blocked;

	if (set) {
		unsigned long s = *(unsigned long *)(unsigned long)set;
		s &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1))); // never blockable
		irq_disable();
		switch (how) {
		case SIG_BLOCK:   self->sig_blocked |= s; break;
		case SIG_UNBLOCK: self->sig_blocked &= ~s; break;
		case SIG_SETMASK: self->sig_blocked = s; break;
		default:
			irq_enable();
			return -EINVAL;
		}
		irq_enable();
	}
	return 0;
}

static long raise_pending(struct bmos_thread *t, long sig)
{
	if (sig < 0 || sig > 64)
		return -EINVAL;
	if (sig == 0)
		return 0; // existence probe -- t was already found/is self, so always "alive"

	irq_disable();
	t->sig_pending |= (1UL << (sig - 1));
	irq_enable();
	return 0;
}

long thread_shim_tkill(long tid, long sig)
{
	// A signal marked pending is only ever actually delivered from the
	// timer tick, which isn't registered until thread_shim_init() runs
	// (normally on the first pthread_create()) -- bootstrap it here too,
	// so raise()/pthread_kill(pthread_self(),...) in a program that
	// never creates a second thread still works. Idempotent.
	thread_shim_init();

	struct bmos_thread *t = find_thread_by_tid((int)tid);
	if (!t)
		return -ESRCH;
	return raise_pending(t, sig);
}

long thread_shim_kill(long pid, long sig)
{
	(void)pid; // see thread_shim.h's comment on this function
	thread_shim_init(); // see thread_shim_tkill()'s comment
	return raise_pending(g_current, sig);
}

int thread_shim_take_eintr(void)
{
	struct bmos_thread *self = (g_active && g_current) ? g_current : &g_main_thread;
	int v = self->sig_eintr;
	self->sig_eintr = 0;
	return v;
}

// =============================================================================
// EOF
