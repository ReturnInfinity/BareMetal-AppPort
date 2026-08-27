// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// Version 1.0
// =============================================================================


#include "libBareMetal.h"

// The app runs in ring 3 (see BareMetal-Firecracker's src/BareMetal/kernel.asm
// start_app); every kernel call below traps in via "int $0x80" with the
// syscall index in R9 (0-6, matching the kernel's b_* function pointer table),
// instead of the plain near `call *0x001000XX` this used to be when the app
// still ran in ring 0. R9 is the only extra clobber needed -- the kernel's
// syscall gate (int_syscall) preserves every other register exactly as the
// direct calls used to.

// Input/Output

u8 b_input(void) {
	u8 chr;
	asm volatile ("movq $0, %%r9\n\tint $0x80" : "=a" (chr) : : "r9");
	return chr;
}

void b_output(const char *str, u64 nbr) {
	asm volatile ("movq $1, %%r9\n\tint $0x80" : : "S"(str), "c"(nbr) : "r9");
}


// Network

void b_net_tx(void *mem, u64 len, u64 iid) {
	asm volatile ("movq $2, %%r9\n\tint $0x80" : : "S"(mem), "c"(len), "d"(iid) : "r9");
}

u64 b_net_rx(void **mem, u64 iid) {
	u64 tlong;
	asm volatile ("movq $3, %%r9\n\tint $0x80" : "=D"(*mem), "=c"(tlong) : "d"(iid) : "r9");
	return tlong;
}


// Non-volatile Storage

u64 b_nvs_read(void *mem, u64 start, u64 num, u64 drivenum) {
	u64 tlong;
	asm volatile ("movq $4, %%r9\n\tint $0x80" : "=c"(tlong) : "a"(start), "c"(num), "d"(drivenum), "D"(mem) : "r9");
	return tlong;
}

u64 b_nvs_write(void *mem, u64 start, u64 num, u64 drivenum) {
	u64 tlong = 0;
	asm volatile ("movq $5, %%r9\n\tint $0x80" : "=c"(tlong) : "a"(start), "c"(num), "d"(drivenum), "S"(mem) : "r9");
	return tlong;
}


// System

u64 b_system(u64 function, u64 var1, u64 var2) {
	u64 tlong;
	asm volatile ("movq $6, %%r9\n\tint $0x80" : "=a"(tlong) : "c"(function), "a"(var1), "d"(var2) : "r9");
	return tlong;
}

// Exit. Syscall index 7 -- the kernel's int_syscall handler treats this one
// specially: it never returns (no iretq back to ring 3). Instead it resets
// RSP to the kernel's own stack and jumps straight to kernel.asm's
// app_finished, exactly as if the app's original `call [app_start]` had
// returned (back when the app still ran in ring 0 on the kernel's stack).
__attribute__((noreturn)) void b_exit(void) {
	asm volatile ("movq $7, %%r9\n\tint $0x80" : : : "r9");
	__builtin_unreachable();
}


// =============================================================================
// EOF
