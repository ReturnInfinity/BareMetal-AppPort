/*
 * libstdc++.a's own precompiled "gnu" locale-model ctype<char> code
 * (config/os/gnu-linux/ctype_configure_char.cc, built into
 * libstdc++.a -- not something recompiled per-app from headers) calls
 * glibc's __ctype_b_loc()/__ctype_tolower_loc()/__ctype_toupper_loc()
 * directly to fetch the classification/case-conversion tables behind
 * every std::ctype<char>::is()/toupper()/tolower() call -- including
 * the two global facets std::cout/std::cin's own sentry/formatting
 * code touches on every single character. musl provides none of this
 * (it computes isupper()/tolower()/etc directly, with no shared
 * exported table), so without this shim, any app that touches
 * <iostream> fails to *link*, not just to compile (see
 * glibc_ctype_compat.h's own header for the compile-time half of this
 * -- the _ISupper/etc bit constants glibc's headers would otherwise
 * provide).
 *
 * This builds real, correct tables from musl's own isupper()/tolower()
 * /etc rather than faking fixed answers -- classification is genuinely
 * right for every byte value, not a stub that happens to satisfy the
 * linker. The one real limitation: this is the "C"/POSIX locale only,
 * always -- there's no locale data on disk anywhere in this port for
 * a real newlocale()/setlocale(LC_ALL, "xx_YY") to load, so non-ASCII
 * classification (accented letters, wide encodings) behaves exactly
 * as it would under `LC_ALL=C` on a real Linux box, not as it would
 * under a real locale.
 *
 * Table layout matches glibc's own: 384 entries, indices -128..255
 * (every possible `char` value, signed or unsigned, plus glibc's own
 * historical EOF-safe padding), with the returned pointer positioned
 * at index 0 so both ptr[-128..-1] and ptr[0..255] are valid.
 */

#include <ctype.h>

#include "glibc_ctype_compat.h"

#define TABLE_SIZE 384
#define TABLE_OFFSET 128

static unsigned short ctype_b_table[TABLE_SIZE];
static int ctype_tolower_table[TABLE_SIZE];
static int ctype_toupper_table[TABLE_SIZE];
static int ctype_tables_ready;

static void init_ctype_tables(void)
{
	for (int i = -128; i < 256; i++) {
		unsigned short mask = 0;

		if (isupper(i)) mask |= _ISupper;
		if (islower(i)) mask |= _ISlower;
		if (isalpha(i)) mask |= _ISalpha;
		if (isdigit(i)) mask |= _ISdigit;
		if (isxdigit(i)) mask |= _ISxdigit;
		if (isspace(i)) mask |= _ISspace;
		if (isprint(i)) mask |= _ISprint;
		if (isgraph(i)) mask |= _ISgraph;
		if (isblank(i)) mask |= _ISblank;
		if (iscntrl(i)) mask |= _IScntrl;
		if (ispunct(i)) mask |= _ISpunct;
		if (isalnum(i)) mask |= _ISalnum;

		ctype_b_table[i + TABLE_OFFSET] = mask;
		ctype_tolower_table[i + TABLE_OFFSET] = tolower(i);
		ctype_toupper_table[i + TABLE_OFFSET] = toupper(i);
	}
	ctype_tables_ready = 1;
}

const unsigned short **__ctype_b_loc(void)
{
	static const unsigned short *table_ptr;
	if (!ctype_tables_ready)
		init_ctype_tables();
	table_ptr = ctype_b_table + TABLE_OFFSET;
	return &table_ptr;
}

const int **__ctype_tolower_loc(void)
{
	static const int *table_ptr;
	if (!ctype_tables_ready)
		init_ctype_tables();
	table_ptr = ctype_tolower_table + TABLE_OFFSET;
	return &table_ptr;
}

const int **__ctype_toupper_loc(void)
{
	static const int *table_ptr;
	if (!ctype_tables_ready)
		init_ctype_tables();
	table_ptr = ctype_toupper_table + TABLE_OFFSET;
	return &table_ptr;
}
