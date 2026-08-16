OUTPUT_FORMAT(binary)
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

SECTIONS
{
	. = 0xFFFF800000000000;
	__image_base = .;

	.text : {
		*(.text .text.*)
	}

	. = ALIGN(16);

	.rodata : {
		*(.rodata .rodata.*)
		/*
		 * -mcmodel=large routes objects too big to reach via a
		 * 32-bit-relative access into .lrodata/.ldata/.lbss instead
		 * of .rodata/.data/.bss (see build-app.sh's CFLAGS comment).
		 * Without these, they'd be left as unclaimed orphan sections
		 * that ld appends after __bss_stop -- outside zero_bss()'s
		 * range and past what posix_shim.c's heap_cur = __bss_stop
		 * treats as the start of free memory, so a large enough
		 * static array and the malloc heap end up aliased onto the
		 * same bytes.
		 */
		*(.lrodata .lrodata.*)
	}

	. = ALIGN(16);

	.data : {
		*(.data .data.*)
		*(.ldata .ldata.*)
	}

	. = ALIGN(16);

	/*
	 * -fdata-sections (build-app.sh's CFLAGS) puts each __thread/
	 * _Thread_local variable in its own uniquely-named .tdata.NAME/
	 * .tbss.NAME input section, same as it already does for .data/
	 * .bss's own per-symbol sections above -- so without an explicit
	 * rule to glob them all into one
	 * output section the way .bss's *(.bss .bss.*) already does,
	 * each one is left an orphan section for ld to place on its own.
	 * Found the hard way: with no rule here at all, ld placed
	 * .tbss._Py_tss_tstate (CPython's "current thread state" pointer)
	 * and .tbss.pkgcontext (CPython's "current package name during an
	 * extension-module import" pointer) at the exact same address --
	 * confirmed via readelf -S, both showing identical VMAs. Once
	 * TWO thread-local variables collide, %fs-relative Local-Exec TLS
	 * access (a compile-time-fixed negative offset from the thread
	 * pointer -- see thread_shim.c's own TLS notes) reads/writes the
	 * SAME memory for what the compiler thinks are two independent
	 * variables: importing any dynamically-loaded extension module
	 * (port/dlfcn_shim.c) overwrites _Py_tss_tstate with pkgcontext's
	 * value (a plain `const char *` module-name string, not a
	 * PyThreadState*) as a side effect of _PyImport_SwapPackageContext
	 * writing to what it thinks is its own separate variable --
	 * corrupting every CPython C-API call the loaded module's own code
	 * makes afterward, well past that one import returning.
	 *
	 * This didn't need a PT_TLS program header or any runtime TLS
	 * setup to go wrong -- Local-Exec offsets are baked in entirely at
	 * link time. musl's own PT_TLS-parsing (Python-3.12.8/../
	 * __init_tls.c's static_init_tls(), walking aux[AT_PHDR]) is
	 * already dead code here regardless (crt0.c's fabricated auxv
	 * carries only AT_PAGESZ/AT_RANDOM -- see this file's _DYNAMIC
	 * comment below), so this fix is purely about giving ld one
	 * correctly-sized, non-overlapping output section per TLS input
	 * section group, mirroring .bss's existing pattern -- nothing
	 * about actually copying/initializing a TLS template at runtime
	 * needs to exist for Local-Exec mode to be correct once the
	 * addresses themselves stop colliding.
	 */
	.tdata : {
		*(.tdata .tdata.*)
	}

	. = ALIGN(16);

	.tbss : {
		*(.tbss .tbss.*)
	}

	. = ALIGN(16);

	/*
	 * musl's startup/exit path (__libc_start_main.c, exit.c) walks
	 * these bounds to run any __attribute__((constructor))/
	 * ((destructor)) functions. Nothing currently uses them, but
	 * they need to be real, in-range symbols: left as weak/
	 * undefined (defaulting to address 0) they cause "relocation
	 * truncated to fit" at link time, since 0 is unreachable via a
	 * 32-bit PC-relative displacement from code way up at
	 * 0xFFFF800000000000.
	 *
	 * KEEP() (not just *(...)) on the array contents themselves: with
	 * build-app.sh's `ld --gc-sections` (see its CFLAGS comment), a
	 * section reachable only by *bounds* symbols pointing at its start
	 * and end -- not a per-entry relocation from other kept code, which
	 * is what a constructor-pointer table looks like -- reads as
	 * unreferenced and gets dropped. Harmless today (nothing populates
	 * these), but silently discarding a real constructor/destructor if
	 * one's ever added later would be a much nastier bug to chase down
	 * than two extra KEEP()s now.
	 */
	.init_array : {
		__init_array_start = .;
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	}

	. = ALIGN(16);

	.fini_array : {
		__fini_array_start = .;
		KEEP(*(.fini_array .dtors))
		__fini_array_end = .;
	}

	. = ALIGN(16);

	__bss_start = .;
	.bss : {
		*(.bss .bss.*)
		*(.lbss .lbss.*)
	}
	__bss_stop = .;

	/*
	 * Referenced (but never dereferenced -- it's only inspected on
	 * a dead ELF-program-header-walking path that can't run here)
	 * by musl's TLS bootstrap. Same "relocation truncated to fit"
	 * reasoning as above.
	 */
	PROVIDE_HIDDEN(_DYNAMIC = .);

	/DISCARD/ : {
		*(.eh_frame*)
		*(.note .note.*)
		*(.intrep)
	}
}
