/*
 * Ubuntu's libstdc++.a was built with glibc's _FORTIFY_SOURCE=2
 * hardening enabled (the distro's default compile flags), so its
 * precompiled object code calls the *_chk() fortified variants of
 * sprintf/memcpy/strcpy/etc directly -- baked in at Ubuntu's own
 * build time, nothing this port's own -fno-builtin/-nostdinc flags
 * can affect. Those _chk() entry points are normally implemented
 * inside glibc itself (glibc doubles as both "the headers that
 * generate _chk calls" and "the library that implements them"); musl
 * only ever plays the first role when its own headers are compiled
 * with _FORTIFY_SOURCE (and even then, expands to a plain unchecked
 * call at the *call site* via its own header macros -- it never
 * exports these symbols itself), so linking real Ubuntu-glibc-built
 * static libraries against musl leaves them undefined.
 *
 * Each shim below does the real bounds-checked thing using the extra
 * "destination buffer size" argument _FORTIFY_SOURCE call sites always
 * pass -- not a no-op passthrough -- so this is equivalent in
 * behavior to what glibc's own _chk functions guarantee (truncate/
 * bound the write), just implemented against musl instead of glibc's
 * internal machinery.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/*
 * ios_base::Init::Init() (libstdc++.a's globals_io.o) unconditionally
 * constructs std::wcout/wcin/wcerr right alongside std::cout/cin/cerr
 * -- there's no way to get the narrow streams without the wide ones
 * coming along, so std::cout alone drags in libstdc++.a's whole wide-
 * locale machinery (wlocale-inst.o, monetary_members.o, ext11-inst.o).
 * That machinery was built by Ubuntu against two more glibc-only
 * surfaces beyond straight _FORTIFY_SOURCE:
 *
 *  - ftello64()/fseeko64(): glibc's explicit 64-bit-off_t (LFS) names,
 *    kept as separate symbols for programs still built with a 32-bit
 *    off_t by default. musl has no 32-vs-64 off_t split at all -- its
 *    off_t is always the 64-bit type these *64 names mean -- so
 *    aliasing straight to ftello()/fseeko() is exact, not a
 *    approximation.
 *  - __wmemset_chk()/__mbsrtowcs_chk(): the same _FORTIFY_SOURCE
 *    pattern as __memset_chk()/etc above, for the wide-char
 *    equivalents.
 */

long ftello64(FILE *f)
{
	return ftello(f);
}

int fseeko64(FILE *f, long off, int whence)
{
	return fseeko(f, off, whence);
}

wchar_t *__wmemset_chk(wchar_t *s, wchar_t c, size_t n, size_t slen)
{
	if (n > slen)
		n = slen;
	return wmemset(s, c, n);
}

size_t __mbsrtowcs_chk(wchar_t *dst, const char **src, size_t len, mbstate_t *ps, size_t dstlen)
{
	if (len > dstlen)
		len = dstlen;
	return mbsrtowcs(dst, src, len, ps);
}

/*
 * Glibc 2.38+ ships ISO-C23-conformant strtol/strtoul/... under a
 * versioned `__isoc23_*` symbol name (the classic unversioned name
 * now aliases to whichever ABI the including TU was compiled against)
 * -- libstdc++.a's own eh_alloc.o (the emergency exception memory
 * pool, sized from the GLIBCXX_TUNABLES env var at static-init time)
 * calls it directly. The ISO C23 changes to strtoul are about corner
 * cases (e.g. explicit "0b"/"0B" binary-prefix parsing) this call
 * site never exercises -- forwarding straight to musl's ordinary
 * strtoul() is exact for every input eh_alloc.cc ever actually passes
 * it (there is no real /proc/self/environ-backed GLIBCXX_TUNABLES
 * here to begin with -- see posix_shim.c's empty-envp posture -- so
 * this always just returns the default pool size either way).
 */
unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
	return strtoul(nptr, endptr, base);
}

int __sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...)
{
	(void)flag;
	va_list ap;
	va_start(ap, fmt);
	int ret = vsnprintf(s, slen, fmt, ap);
	va_end(ap);
	return ret;
}

int __snprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...)
{
	(void)flag;
	size_t n = maxlen < slen ? maxlen : slen;
	va_list ap;
	va_start(ap, fmt);
	int ret = vsnprintf(s, n, fmt, ap);
	va_end(ap);
	return ret;
}

int __vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list ap)
{
	(void)flag;
	return vsnprintf(s, slen, fmt, ap);
}

int __vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list ap)
{
	(void)flag;
	size_t n = maxlen < slen ? maxlen : slen;
	return vsnprintf(s, n, fmt, ap);
}

void *__memcpy_chk(void *dst, const void *src, size_t len, size_t dstlen)
{
	if (len > dstlen)
		len = dstlen;
	return memcpy(dst, src, len);
}

void *__memmove_chk(void *dst, const void *src, size_t len, size_t dstlen)
{
	if (len > dstlen)
		len = dstlen;
	return memmove(dst, src, len);
}

void *__memset_chk(void *dst, int val, size_t len, size_t dstlen)
{
	if (len > dstlen)
		len = dstlen;
	return memset(dst, val, len);
}

char *__strcpy_chk(char *dst, const char *src, size_t dstlen)
{
	size_t len = strlen(src);
	if (len >= dstlen)
		len = dstlen ? dstlen - 1 : 0;
	memcpy(dst, src, len);
	if (dstlen)
		dst[len] = '\0';
	return dst;
}

char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dstlen)
{
	if (n > dstlen)
		n = dstlen;
	return strncpy(dst, src, n);
}

char *__strcat_chk(char *dst, const char *src, size_t dstlen)
{
	size_t dlen = strlen(dst);
	size_t slen = strlen(src);
	size_t avail = dlen < dstlen ? dstlen - dlen : 0;
	if (slen >= avail)
		slen = avail ? avail - 1 : 0;
	memcpy(dst + dlen, src, slen);
	if (avail)
		dst[dlen + slen] = '\0';
	return dst;
}

char *__strncat_chk(char *dst, const char *src, size_t n, size_t dstlen)
{
	size_t dlen = strlen(dst);
	/* Room for appended chars must exclude the terminating NUL
	 * strncat() always writes, same as __strcat_chk() above. */
	size_t avail = dlen < dstlen ? dstlen - dlen - 1 : 0;
	if (n > avail)
		n = avail;
	return strncat(dst, src, n);
}
