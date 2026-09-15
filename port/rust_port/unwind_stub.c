/*
 * std::backtrace / panic printing references libgcc's or libunwind's
 * _Unwind_* API to walk stack frames. c.ld discards .eh_frame (no
 * unwind tables exist anywhere in this port -- every Rust app here is
 * built with panic=abort, matching the C side's own no-exceptions,
 * no-.eh_frame posture), so there is nothing for a real unwinder to
 * walk and nothing normally provides libgcc_s/libunwind here either.
 *
 * These are link-time stubs only: they satisfy the reference so the
 * final link succeeds, and report "nothing found" (matching the
 * _Unwind_Reason_Code contract's own failure codes) rather than ever
 * being called with real intent -- std's backtrace path degrades to
 * "unavailable" at runtime, it does not crash. Confirmed via a POC
 * build: these are the only symbols a `std,panic_abort` build-std
 * needs beyond what posix_shim.c/libBareMetal.c/musl's own libc.a
 * already provide.
 */

typedef int _Unwind_Reason_Code;
typedef void _Unwind_Context;

#define _URC_NO_REASON 0
#define _URC_END_OF_STACK 5

_Unwind_Reason_Code _Unwind_Backtrace(void *trace, void *trace_argument)
{
	(void)trace; (void)trace_argument;
	return _URC_END_OF_STACK;
}

unsigned long _Unwind_GetIP(_Unwind_Context *ctx) { (void)ctx; return 0; }

unsigned long _Unwind_GetIPInfo(_Unwind_Context *ctx, int *ip_before_insn)
{
	(void)ctx;
	if (ip_before_insn)
		*ip_before_insn = 0;
	return 0;
}

unsigned long _Unwind_GetCFA(_Unwind_Context *ctx) { (void)ctx; return 0; }
unsigned long _Unwind_GetDataRelBase(_Unwind_Context *ctx) { (void)ctx; return 0; }
unsigned long _Unwind_GetTextRelBase(_Unwind_Context *ctx) { (void)ctx; return 0; }
unsigned long _Unwind_GetRegionStart(_Unwind_Context *ctx) { (void)ctx; return 0; }
void *_Unwind_GetLanguageSpecificData(_Unwind_Context *ctx) { (void)ctx; return (void *)0; }

_Unwind_Reason_Code _Unwind_SetGR(_Unwind_Context *ctx, int index, unsigned long value)
{
	(void)ctx; (void)index; (void)value;
	return _URC_NO_REASON;
}

_Unwind_Reason_Code _Unwind_SetIP(_Unwind_Context *ctx, unsigned long value)
{
	(void)ctx; (void)value;
	return _URC_NO_REASON;
}

void *_Unwind_FindEnclosingFunction(void *pc) { (void)pc; return (void *)0; }
