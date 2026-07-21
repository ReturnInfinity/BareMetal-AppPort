#ifndef _POSIX_SHIM_H
#define _POSIX_SHIM_H

// The dispatcher musl's syscall transport (arch/x86_64/syscall_arch.h
// and src/thread/x86_64/syscall_cp.s) calls instead of trapping via
// the `syscall` instruction. `n` is a Linux x86-64 syscall number
// (SYS_* from musl's <sys/syscall.h>), used only as a dispatch key.
// Returns the same convention a real Linux syscall would: the result
// on success, or a small negative -errno on failure.
long __bmos_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

#endif
