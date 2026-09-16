/*
 * C++ apps here are built with -fno-exceptions -fno-rtti (see
 * build-cpp-app.sh) -- the same panic=abort posture already used for
 * Rust (RUST.md) and matching the C side's own no-.eh_frame stance
 * (port/c.ld /DISCARD/s .eh_frame* entirely; there is no unwinder
 * anywhere in this port).
 *
 * That flag only stops *app* code from containing try/throw/catch --
 * it does nothing about libstdc++.a itself. The host's libstdc++.a
 * (Ubuntu's, built against glibc) is compiled normally, with real
 * exceptions throughout: std::vector::at()'s bounds check, operator
 * new's out-of-memory path, std::string's length checks, etc. all
 * eventually call libstdc++'s own internal std::__throw_*() helpers
 * (bits/functexcept.h), which for real would call __cxa_throw() ->
 * _Unwind_RaiseException() -> walk .eh_frame looking for a landing
 * pad. None of that machinery can work here.
 *
 * Fix: override every std::__throw_*() helper with a plain (non-
 * throwing) definition here, linked as an ordinary .o *before*
 * libstdc++.a on build-cpp-app.sh's link line. Standard archive
 * resolution only pulls an .a member in to satisfy a symbol that's
 * still undefined when the linker reaches it -- since these are
 * already defined by the time ld gets to libstdc++.a, the real
 * (throwing) implementations in functexcept.o never get linked in at
 * all. Each override here reports the failure via b_output() and
 * halts via b_exit(), the same fatal-error style crt0.c's
 * image_fits_in_ram() already uses -- this is a hard abort, not a
 * real exception: control never returns to the caller.
 *
 * operator new/delete are overridden the same way, for a different
 * reason: libsupc++'s own default operator new (libsupc++/new_op.cc)
 * calls std::get_new_handler() and throws std::bad_alloc() on failure
 * -- pulling in the exact throw path above just from a plain `new`
 * expression. Routing new/delete through musl's malloc/free directly
 * (already provided by $MUSL_LIB -- see build-cpp-app.sh) avoids
 * pulling in new_op.o at all.
 *
 * The _Unwind_* / __cxa_throw / __gxx_personality_v0 stubs below are a
 * last-resort safety net, not the primary mechanism: they exist in
 * case something inside libstdc++ (locale/iostream/ios_base failure
 * paths in particular) throws directly rather than going through a
 * std::__throw_*() helper. Modeled directly on
 * port/rust_port/unwind_stub.c's _Unwind_* stubs, extended with the
 * handful of Itanium C++ ABI entry points __cxa_throw() itself needs
 * that Rust's panic=abort path never touches. Unlike unwind_stub.c's
 * "report nothing found, degrade gracefully" contract (fine for a
 * best-effort backtrace), __cxa_throw() reaching here means a real
 * C++ exception was about to propagate with nowhere to go -- so this
 * aborts instead of returning a fabricated "no handler" status that
 * would leave the caller to do who-knows-what next.
 */

#include <cstddef>
#include <new>

/* libBareMetal.h has no __cplusplus/extern "C" guard of its own (it's
 * shared with every plain-C app in this port) -- wrap the include so
 * b_output()/b_exit() keep C linkage instead of getting C++-mangled
 * names that wouldn't match the .o build-app.sh's own C build of
 * libBareMetal.c produces. */
extern "C" {
#include "libBareMetal.h"
}

extern "C" void *malloc(size_t size);
extern "C" void free(void *ptr);

namespace {

/* No sprintf/snprintf available this early/this minimally -- same
 * hand-rolled-output posture crt0.c's own fatal-error path takes
 * (see its image_fits_in_ram()/out_str()/u64_to_dec()). */
[[noreturn]] void fatal(const char *msg)
{
	const char *p = msg;
	size_t len = 0;
	while (p[len] != '\0')
		len++;
	b_output(msg, (u64)len);
	b_exit();
}

} // namespace

/*
 * operator new/delete: route straight through musl's malloc/free,
 * never through libsupc++'s own throwing default (see file header).
 * All eight forms (new, new[], nothrow new, nothrow new[], and the
 * matching four deletes, sized and unsized) are provided so nothing
 * pulls the real ones out of libstdc++.a/libsupc++.a instead.
 */
void *operator new(std::size_t size)
{
	void *p = malloc(size ? size : 1);
	if (!p)
		fatal("cxxabi_stub: operator new: out of memory\n");
	return p;
}

void *operator new[](std::size_t size)
{
	return operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	return malloc(size ? size : 1);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
	return malloc(size ? size : 1);
}

void operator delete(void *ptr) noexcept
{
	free(ptr);
}

void operator delete[](void *ptr) noexcept
{
	free(ptr);
}

void operator delete(void *ptr, std::size_t) noexcept
{
	free(ptr);
}

void operator delete[](void *ptr, std::size_t) noexcept
{
	free(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
	free(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept
{
	free(ptr);
}

/*
 * libstdc++'s internal throw helpers (bits/functexcept.h) -- every
 * container/string/iostream bounds/length/allocation check funnels
 * through one of these instead of a bare `throw` in the caller's own
 * object code. Overriding this layer (rather than __cxa_throw itself)
 * covers the overwhelming majority of libstdc++-internal throw sites
 * without needing to fake exception *propagation* semantics at all --
 * each of these is documented [[noreturn]] in libstdc++ itself, so a
 * hard abort here is a legal (if fatal) implementation.
 */
namespace std {

void __throw_bad_exception()
{ fatal("cxxabi_stub: std::bad_exception\n"); }

void __throw_bad_alloc()
{ fatal("cxxabi_stub: std::bad_alloc\n"); }

void __throw_bad_array_new_length()
{ fatal("cxxabi_stub: std::bad_array_new_length\n"); }

void __throw_bad_cast()
{ fatal("cxxabi_stub: std::bad_cast\n"); }

void __throw_bad_typeid()
{ fatal("cxxabi_stub: std::bad_typeid\n"); }

void __throw_logic_error(const char *)
{ fatal("cxxabi_stub: std::logic_error\n"); }

void __throw_domain_error(const char *)
{ fatal("cxxabi_stub: std::domain_error\n"); }

void __throw_invalid_argument(const char *)
{ fatal("cxxabi_stub: std::invalid_argument\n"); }

void __throw_length_error(const char *)
{ fatal("cxxabi_stub: std::length_error\n"); }

void __throw_out_of_range(const char *)
{ fatal("cxxabi_stub: std::out_of_range\n"); }

void __throw_out_of_range_fmt(const char *, ...)
{ fatal("cxxabi_stub: std::out_of_range\n"); }

void __throw_runtime_error(const char *)
{ fatal("cxxabi_stub: std::runtime_error\n"); }

void __throw_range_error(const char *)
{ fatal("cxxabi_stub: std::range_error\n"); }

void __throw_overflow_error(const char *)
{ fatal("cxxabi_stub: std::overflow_error\n"); }

void __throw_underflow_error(const char *)
{ fatal("cxxabi_stub: std::underflow_error\n"); }

void __throw_ios_failure(const char *)
{ fatal("cxxabi_stub: std::ios_base::failure\n"); }

void __throw_ios_failure(const char *, int)
{ fatal("cxxabi_stub: std::ios_base::failure\n"); }

/* __throw_system_error() deliberately NOT overridden here -- see the
 * big comment on the _Unwind_* / __cxa_* stubs below for why: it lives
 * in libstdc++.a's system_error.o, which <ios> pulls in for real
 * regardless, and a real throw from it still dead-ends safely in this
 * file's own _Unwind_RaiseException() stub (nothing else in this link
 * provides libgcc_eh, so that's where any real __cxa_throw() call
 * lands no matter what constructed the exception object). */

void __throw_future_error(int)
{ fatal("cxxabi_stub: std::future_error\n"); }

void __throw_bad_function_call()
{ fatal("cxxabi_stub: std::bad_function_call\n"); }

} // namespace std

/* Pure virtual call with no override reachable -- real GCC ABI entry
 * point, always weakly expected to exist; abort, matching every other
 * "this should be structurally impossible" path in this file. */
extern "C" void __cxa_pure_virtual()
{ fatal("cxxabi_stub: pure virtual function call\n"); }

extern "C" void __cxa_deleted_virtual()
{ fatal("cxxabi_stub: deleted virtual function call\n"); }

/*
 * Last-resort safety net (see file header): if anything ever reaches
 * an actual throw/unwind entry point despite the __throw_*() overrides
 * above, abort here instead of corrupting state by pretending to
 * unwind. Modeled on port/rust_port/unwind_stub.c's _Unwind_* stubs.
 *
 * NOT overridden here, deliberately: __cxa_throw/__cxa_begin_catch/
 * __cxa_end_catch/__gxx_personality_v0/std::__throw_system_error.
 * Those live inside libstdc++.a's own eh_throw.o/eh_catch.o/
 * eh_personality.o/system_error.o -- and <ios>'s own machinery
 * (ios_base::failure derives from system_error) pulls at least one of
 * those .o's in for real, for other symbols inside them, regardless
 * of whether an app ever actually triggers a throw. Once ld pulls a
 * whole .o in from the archive for symbol A, every OTHER symbol that
 * same .o defines -- including __cxa_begin_catch, say -- becomes a
 * real, strong definition too; overriding it here as well produced a
 * hard "multiple definition" link error, not a harmless shadow (a
 * .o file listed directly on the command line, unlike an archive
 * member, is never conditionally skipped). Leaving these five
 * unshimmed lets the real ones link in from libstdc++.a instead --
 * confirmed dead code as long as nothing actually throws (every
 * *reachable* throw site in ordinary use -- vector::at(), bad_alloc,
 * string length checks -- is already intercepted earlier by this
 * file's std::__throw_*() overrides above, which abort before ever
 * constructing an exception object or calling __cxa_throw at all).
 * See CPP.md's "Known gaps" for the real residual risk this leaves:
 * a raw `throw` compiled directly into some libstdc++.a internal (not
 * routed through a std::__throw_*() helper -- locale/facet edge cases
 * are the most likely place) would still reach the real __cxa_throw
 * and attempt a real unwind with no .eh_frame data anywhere in this
 * port to walk.
 */
extern "C" {

typedef int _Unwind_Reason_Code;
typedef void _Unwind_Context;
typedef void _Unwind_Exception;

_Unwind_Reason_Code _Unwind_RaiseException(_Unwind_Exception *)
{ fatal("cxxabi_stub: _Unwind_RaiseException called -- no unwinder in this port\n"); }

void _Unwind_Resume(_Unwind_Exception *)
{ fatal("cxxabi_stub: _Unwind_Resume called -- no unwinder in this port\n"); }

void _Unwind_DeleteException(_Unwind_Exception *)
{ fatal("cxxabi_stub: _Unwind_DeleteException called -- no unwinder in this port\n"); }

_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(_Unwind_Exception *)
{ fatal("cxxabi_stub: _Unwind_Resume_or_Rethrow called -- no unwinder in this port\n"); }

unsigned long _Unwind_GetIP(_Unwind_Context *) { return 0; }
unsigned long _Unwind_GetIPInfo(_Unwind_Context *, int *ip_before_insn)
{
	if (ip_before_insn)
		*ip_before_insn = 0;
	return 0;
}
unsigned long _Unwind_GetCFA(_Unwind_Context *) { return 0; }
unsigned long _Unwind_GetDataRelBase(_Unwind_Context *) { return 0; }
unsigned long _Unwind_GetTextRelBase(_Unwind_Context *) { return 0; }
unsigned long _Unwind_GetRegionStart(_Unwind_Context *) { return 0; }
void *_Unwind_GetLanguageSpecificData(_Unwind_Context *) { return nullptr; }
_Unwind_Reason_Code _Unwind_SetGR(_Unwind_Context *, int, unsigned long) { return 0; }
_Unwind_Reason_Code _Unwind_SetIP(_Unwind_Context *, unsigned long) { return 0; }
void *_Unwind_FindEnclosingFunction(void *) { return nullptr; }

_Unwind_Reason_Code _Unwind_Backtrace(void *, void *) { return 5 /* _URC_END_OF_STACK */; }

} // extern "C"
