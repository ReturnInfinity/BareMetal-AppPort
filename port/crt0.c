extern int main(int argc, char **argv, char **envp);
extern int __libc_start_main(int (*)(int, char **, char **), int, char **,
                              void (*)(), void (*)(), void (*)());

extern char __bss_start;
extern char __bss_stop;

static void zero_bss(void);
static void fill_random(unsigned char buf[16]);
static int has_rdrand(void);

#define AT_NULL		0
#define AT_PAGESZ	6
#define AT_RANDOM	25

/*
 * Ensure RSP is 16-byte aligned. SSE instructions such as
 * MOVAPS will #GP on the mis-aligned stack.
 */
__attribute__((naked)) void _start(void)
{
	__asm__ volatile (
		"pushq %%rbp\n\t"        /* save rbp (callee-saved)     */
		"movq %%rsp, %%rbp\n\t"  /* remember original RSP       */
		"andq $-16, %%rsp\n\t"   /* ensure 16-byte alignment    */
		"call _start_c\n\t"      /* CALL so RSP is 8-mod-16 inside _start_c */
		"movq %%rbp, %%rsp\n\t"  /* restore original RSP        */
		"popq %%rbp\n\t"         /* restore rbp                 */
		"ret\n\t"                /* return to BareMetal OS       */
		::: "memory"
	);
}

int _start_c(void)
{
	zero_bss();

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
