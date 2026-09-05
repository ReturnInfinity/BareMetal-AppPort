#include <stddef.h>

#include "libBareMetal.h"

extern int main(int argc, char **argv, char **envp);
extern int __libc_start_main(int (*)(int, char **, char **), int, char **,
                              void (*)(), void (*)(), void (*)());

extern char __bss_start;
extern char __bss_stop;
extern char __image_base[];

static int image_fits_in_ram(void);
static void zero_bss(void);
static void fill_random(unsigned char buf[16]);
static int has_rdrand(void);

#define AT_NULL		0
#define AT_PAGESZ	6
#define AT_RANDOM	25

// Firecracker writes the whole kernel cmdline here for guests that have
// no other way to read it (see net_glue.c's FC_IP_PARAM_ADDR -- same
// address, same 256-byte reserved region per BareMetal-Firecracker's
// memory map, this just looks for a different token in it). baremetal.sh
// sets boot_args to exactly `args=\`$*\`` when args are passed to
// 2-run.sh, e.g.:
//   args=`This is a test`
// -> argv = { "main", "This", "is", "a", "test" }
// argv[0] is always the literal "main" -- BareMetal apps have no
// filename of their own the way a Linux argv[0] would carry one.
#define FC_ARGS_PARAM_ADDR ((const char *)0x5a00UL)
#define FC_ARGS_PARAM_MAXLEN 256
#define FC_ARGS_MAX_ARGC 32	/* argv[0] ("main") + up to 31 args from the cmdline */

/*
 * Ensure RSP is 16-byte aligned. SSE instructions such as
 * MOVAPS will #GP on the mis-aligned stack.
 */
__attribute__((naked)) void _start(void)
{
	__asm__ volatile (
		"pushq %%rbp\n\t"        /* save rbp (callee-saved)     */
		"movq %%rsp, %%rbp\n\t"  /* remember original RSP       */
		"movq %%rbp, %%rdi\n\t"  /* ...and pass it to _start_c  */
		"andq $-16, %%rsp\n\t"   /* ensure 16-byte alignment    */
		"call _start_c\n\t"      /* CALL so RSP is 8-mod-16 inside _start_c */
		"movq %%rbp, %%rsp\n\t"  /* restore original RSP        */
		"popq %%rbp\n\t"         /* restore rbp                 */
		"ret\n\t"                /* return to BareMetal OS       */
		::: "memory"
	);
}

// Copies the NUL-terminated string at FC_ARGS_PARAM_ADDR (capped at
// FC_ARGS_PARAM_MAXLEN, in case it's uninitialized/non-Firecracker memory
// with no NUL in range), finds the "args=`...`" token in it as a
// Linux-style kernel command line (the same way net_glue.c's
// fc_parse_ip_param() finds "ip=" -- as a whole token, at the start of
// the line or preceded by whitespace, not assumed to be a prefix of the
// buffer), and splits the backtick-quoted string on whitespace into
// argv[1..]. argv[0] is always "main". Returns argc (>= 1); on a missing
// or malformed "args=" token, argv is just { "main" }.
//
// No libc yet at this point in startup (this runs before
// __libc_start_main() -- see the comment on fill_random()), hence
// hand-rolled rather than using strstr/strtok.
static int fc_parse_args_param(char **argv, int max_argc)
{
	static char buf[FC_ARGS_PARAM_MAXLEN];
	const char *src = FC_ARGS_PARAM_ADDR;
	int argc = 0;
	size_t len;

	argv[argc++] = "main";

	for (len = 0; len < sizeof(buf) - 1 && src[len] != '\0'; len++)
		buf[len] = src[len];
	buf[len] = '\0';

	char *tok = NULL;
	for (char *p = buf; *p != '\0'; p++) {
		if ((p == buf || p[-1] == ' ' || p[-1] == '\t') &&
		    p[0] == 'a' && p[1] == 'r' && p[2] == 'g' && p[3] == 's' && p[4] == '=') {
			tok = p + 5;
			break;
		}
	}

	if (tok == NULL || *tok != '`')
		return argc;	/* no "args=" token (or malformed): argv = {"main"} */

	tok++;	/* skip opening backtick */

	char *end = tok;
	while (*end != '\0' && *end != '`')
		end++;
	if (*end != '`')
		return argc;	/* no closing backtick found: truncated/malformed, ignore */
	*end = '\0';

	/* Split the backtick-quoted string on whitespace in place. */
	char *p = tok;
	while (*p != '\0' && argc < max_argc) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;
		argv[argc++] = p;
		while (*p != '\0' && *p != ' ' && *p != '\t')
			p++;
		if (*p != '\0')
			*p++ = '\0';
	}

	return argc;
}

int _start_c(void *entry_sp)
{
	(void)entry_sp;	/* only used to keep the CALL into here 8-mod-16 aligned (see _start) */

	/*
	 * zero_bss() below writes every byte from __bss_start through
	 * __bss_stop -- the whole app image's footprint (.text+.rodata+
	 * .data+.bss, see c.ld), not just "the app's own code" -- and all
	 * of it has to be real, mapped memory for that loop to land in.
	 * BareMetal maps exactly b_system(FREE_MEMORY, ...) MiB of RAM for
	 * this app starting at __image_base (the same bound
	 * posix_shim.c's heap_init() later treats as the ceiling on
	 * brk()/mmap()); if __bss_stop falls past that -- easy to hit on
	 * a small VM now that lwIP+mbedTLS+curl are statically linked into
	 * every app regardless of whether it uses them (see build-app.sh)
	 * -- zero_bss() would silently write straight past the mapped
	 * window into unmapped memory. That's a bare page fault deep
	 * inside crt0.c, before main() (or even __libc_start_main()) ever
	 * runs, with nothing to say why. Checking first, before anything
	 * touches memory it doesn't own, turns that into a clear message
	 * on the console instead.
	 */
	if (!image_fits_in_ram())
		b_exit();	/* never returns -- _start has no way to `ret` into the kernel anymore */

	zero_bss();

	static unsigned char randbuf[16];
	fill_random(randbuf);

	/*
	 * musl's real startup path (__libc_start_main -> __init_tls ->
	 * exit()) expects a Linux-style initial stack: argc, argv
	 * (NULL-terminated), envp (NULL-terminated), then an auxv
	 * array of {key,value} pairs terminated by {AT_NULL,0}.
	 * BareMetal doesn't hand the app anything like this -- it just
	 * calls _start() -- so it's fabricated here. argv comes from
	 * Firecracker's "args=" cmdline token (see fc_parse_args_param())
	 * and envp is always empty; the auxv only carries the two entries
	 * musl's startup actually consumes: AT_PAGESZ (mallocng divides by
	 * this -- a zero here breaks it) and AT_RANDOM (stack
	 * protector / malloc hardening entropy).
	 */
	static char *fc_argv[FC_ARGS_MAX_ARGC];
	int fc_argc = fc_parse_args_param(fc_argv, FC_ARGS_MAX_ARGC);

	static long init_stack[FC_ARGS_MAX_ARGC + 8];
	int idx = 0;
	for (int i = 0; i < fc_argc; i++)
		init_stack[idx++] = (long)fc_argv[i];
	init_stack[idx++] = 0;                     /* argv[] terminator */
	init_stack[idx++] = 0;                     /* envp[0] terminator (empty envp) */
	init_stack[idx++] = AT_PAGESZ; init_stack[idx++] = 4096;
	init_stack[idx++] = AT_RANDOM; init_stack[idx++] = (long)randbuf;
	init_stack[idx++] = AT_NULL;   init_stack[idx++] = 0;

	return __libc_start_main(main, fc_argc, (char **)init_stack, 0, 0, 0);
}

/* Renders v in decimal into the tail of buf (which must be at least
 * 20 bytes -- enough for a u64's worst case), returning a pointer to
 * the first digit written. No libc yet at this point in startup (this
 * runs before __libc_start_main()), hence hand-rolled. */
static char *u64_to_dec(u64 v, char *buf_end)
{
	char *p = buf_end;
	*--p = '\0';
	do {
		*--p = '0' + (char)(v % 10);
		v /= 10;
	} while (v);
	return p;
}

static void out_str(char **p, const char *s)
{
	while (*s)
		*(*p)++ = *s++;
}

static int image_fits_in_ram(void)
{
	u64 need_bytes = (u64)&__bss_stop - (u64)__image_base;
	u64 have_mib = b_system(FREE_MEMORY, 0, 0);
	u64 need_mib = (need_bytes + 1024 * 1024 - 1) / (1024 * 1024); /* round up */

	if (need_mib <= have_mib)
		return 1;

	/* Stack-local, deliberately not static: this runs before
	 * zero_bss() -- and *because* the image doesn't fit in RAM, static
	 * storage (anywhere in .bss, not just past the ceiling) isn't
	 * provably safe to touch yet. The stack itself is a separate,
	 * always-valid region BareMetal sets up independently of the
	 * app's own image size (it's the top slice of the same
	 * high-mapped RAM window, sized off the VM's total RAM rather
	 * than off this image), so it's the only storage this function
	 * can trust before the check below has passed. */
	char msg[192];
	char numbuf[20];
	char *p = msg;

	out_str(&p, "crt0: app image needs ~");
	out_str(&p, u64_to_dec(need_mib, numbuf + sizeof(numbuf)));
	out_str(&p, " MiB of RAM but this VM only has ");
	out_str(&p, u64_to_dec(have_mib, numbuf + sizeof(numbuf)));
	out_str(&p, " MiB -- give the VM more memory.\n");

	b_output(msg, (u64)(p - msg));
	return 0;
}

static void zero_bss(void)
{
	for (char *c = &__bss_start; c < &__bss_stop; c++)
		*c = 0;
}

static int has_rdrand(void)
{
	unsigned int eax, ebx, ecx, edx;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
	return (ecx >> 30) & 1;
}

static void fill_random(unsigned char buf[16])
{
	int rdrand_ok = has_rdrand();

	for (int i = 0; i < 2; i++) {
		unsigned long v = 0;
		int ok = 0;

		if (rdrand_ok) {
			for (int tries = 0; tries < 10 && !ok; tries++)
				__asm__ volatile ("rdrand %0\n\tsetc %b1" : "=r"(v), "=q"(ok) :: "cc");
		}

		if (!ok) {
			/* No RDRAND (or it kept failing): fall back to a
			 * non-cryptographic mix. Only used to seed the
			 * stack-protector canary / malloc hardening, not
			 * for anything security-critical. */
			unsigned int lo, hi;
			__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
			v = ((unsigned long)hi << 32 | lo) ^ (unsigned long)buf ^ (unsigned long)i;
		}

		__builtin_memcpy(buf + i * 8, &v, 8);
	}
}
