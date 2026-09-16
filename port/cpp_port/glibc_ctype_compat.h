/*
 * libstdc++'s bits/ctype_base.h (the "gnu" locale model, which this
 * GCC install is built for) hard-codes std::ctype_base::mask values
 * directly as glibc's own internal _ISupper/_ISlower/... classification
 * bits (see /usr/include/x86_64-linux-gnu/c++/15/bits/ctype_base.h) --
 * normally supplied transitively by glibc's own <ctype.h>. musl's
 * <ctype.h> has no notion of these at all (musl's isupper()/etc are
 * ordinary functions/macros, not a shared exported bitmask table), so
 * with -nostdinc routing every C header through musl instead, nothing
 * ever defines them.
 *
 * The bit *values* below are glibc's real, stable, publicly documented
 * ABI (glibc's own bits/ctype-classes.h/_ISbit() macro) -- reproducing
 * them here is exact, not an approximation: any code that ORs/tests
 * these masks against a table built with the same _ISbit() layout
 * (see glibc_ctype_shim.c's __ctype_b_loc()) gets the identical
 * answer glibc itself would give.
 *
 * Forced into every C++ TU via build-cpp-app.sh's `-include` (must be
 * defined before bits/ctype_base.h is ever parsed; there's no single
 * header of our own that both app code and libstdc++ headers already
 * funnel through the way there would be with a real sysroot).
 */
#ifndef _ISbit
#define _ISbit(bit) ((bit) < 8 ? ((1 << (bit)) << 8) : ((1 << (bit)) >> 8))
#endif

#ifndef _ISupper
#define _ISupper	_ISbit(0)	/* UPPERCASE.  */
#define _ISlower	_ISbit(1)	/* lowercase.  */
#define _ISalpha	_ISbit(2)	/* Alphabetic.  */
#define _ISdigit	_ISbit(3)	/* Numeric.  */
#define _ISxdigit	_ISbit(4)	/* Hexadecimal numeric.  */
#define _ISspace	_ISbit(5)	/* Whitespace.  */
#define _ISprint	_ISbit(6)	/* Printing.  */
#define _ISgraph	_ISbit(7)	/* Graphical.  */
#define _ISblank	_ISbit(8)	/* Blank (usually SPACE and TAB).  */
#define _IScntrl	_ISbit(9)	/* Control character.  */
#define _ISpunct	_ISbit(10)	/* Punctuation.  */
#define _ISalnum	_ISbit(11)	/* Alphanumeric.  */
#endif
