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

/*
 * RSP as it was when BareMetal `call`ed into _start, captured in
 * _start's prologue and handed to _start_c as an argument rather
 * than written straight to this global from _start: it lives in
 * .bss, and _start_c's first act is zero_bss(), which would
 * immediately wipe it again if it were set any earlier. exit()/
 * _exit() never return up through the normal call chain (musl's
 * _Exit() spins forever on the syscall instead) -- so sys_exit()
 * (posix_shim.c) restores RSP from this and issues its own "pop
 * rbp; ret" to unwind straight back to _start's tail in one shot,
 * exactly as if main() had returned normally. That's what lets the
 * app just fall out to BareMetal, which shuts down once _start
 * returns.
 */
void *__bmos_entry_sp;

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

int _start_c(void *entry_sp)
{
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
		return 1;

	zero_bss();

	__bmos_entry_sp = entry_sp;	/* safe now that .bss is zeroed */

	static unsigned char randbuf[16];
	fill_random(randbuf);

	/*
	 * musl's real startup path (__libc_start_main -> __init_tls ->
	 * exit()) expects a Linux-style initial stack: argc, argv
	 * (NULL-terminated), envp (NULL-terminated), then an auxv
	 * array of {key,value} pairs terminated by {AT_NULL,0}.
	 * BareMetal doesn't hand the app anything like this -- it just
	 * calls _start() -- so it's fabricated here. argc is 0 and
	 * envp is empty; the auxv only carries the two entries musl's
	 * startup actually consumes: AT_PAGESZ (mallocng divides by
	 * this -- a zero here breaks it) and AT_RANDOM (stack
	 * protector / malloc hardening entropy).
	 */
	static long init_stack[] = {
		0,                     /* argv[0] terminator (argc = 0) */
		0,                     /* envp[0] terminator (empty envp) */
		AT_PAGESZ, 4096,
		AT_RANDOM, (long)randbuf,
		AT_NULL, 0,
	};

	return __libc_start_main(main, 0, (char **)init_stack, 0, 0, 0);
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
	 * image's own mapped window (entry_sp is already a valid low
	 * address by the time _start_c runs), so it's the only storage
	 * this function can trust before the check below has passed. */
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
