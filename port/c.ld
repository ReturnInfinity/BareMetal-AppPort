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
	 * link time. musl's own PT_TLS-parsing (Python-3.14.7/../
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
	/*
	 * __tdata_start/__tdata_end/__tbss_end: crt0.c fabricates a
	 * single-entry PT_TLS program header from these (see its own
	 * comment) so musl's static_init_tls()/__copy_tls() -- otherwise
	 * dead code here, since there's no real program header table left
	 * once c.ld's own OUTPUT_FORMAT(binary) strips it -- actually
	 * copies this template into every thread's TLS block instead of
	 * leaving it (and therefore every #[thread_local]/__thread
	 * variable with a non-zero initializer) uninitialized.
	 */
	__tdata_start = .;

	.tdata : {
		*(.tdata .tdata.*)
	}

	__tdata_end = .;

	. = ALIGN(16);

	.tbss : {
		*(.tbss .tbss.*)
	}

	/*
	 * ld's location counter famously does *not* advance past a TLS
	 * (SHT_TLS) output section's own size the way it does for every
	 * other section -- .tbss's PT_TLS phdr entry (readelf -l) reports
	 * the right p_memsz regardless, but a bare `symbol = .;` placed
	 * right after this block would otherwise land at .tbss's *start*,
	 * not its end (confirmed: without this line, __tbss_end below came
	 * out equal to __tdata_end). SIZEOF() is ld's own documented
	 * workaround for exactly this.
	 */
	. = . + SIZEOF(.tbss);

	__tbss_end = .;

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
	 * unreferenced and gets dropped.
	 *
	 * *(.init_array .init_array.* .ctors .ctors.*) -- the wildcard
	 * suffix forms are load-bearing, not defensive: with
	 * -ffunction-sections (every build-*-app.sh's CFLAGS), gcc doesn't
	 * always emit a plain `.init_array` for a translation unit's own
	 * static-initializer entry -- it names it `.init_array.NNNNN`
	 * (NNNNN encoding init priority) instead, the exact same pattern
	 * -fdata-sections already causes for .tdata/.tbss (see that
	 * section's own comment above). A bare `*(.init_array .ctors)`
	 * rule -- this section's own original form -- silently leaves any
	 * such entry as an unmatched orphan section, placed by ld
	 * *outside* [__init_array_start, __init_array_end), so
	 * __libc_start_main.c's own bounds-walking loop never calls it at
	 * all. Found the hard way building the C++ port: libstdc++.a's
	 * globals_io.o carries std::cout/cin/cerr's real constructor in
	 * exactly a `.init_array.00090` section -- with the un-suffixed
	 * rule, it silently never ran, leaving std::cout as zeroed .bss
	 * (no vtable pointer) and the first real `std::cout << ...` call
	 * segfaulting on a null vtable read. No app happened to trip this
	 * before the C++ port because nothing here previously relied on a
	 * *library-provided* (not app-local) global constructor -- see
	 * CPP.md.
	 *
	 * SORT_BY_INIT_PRIORITY on the suffixed forms, matching the
	 * standard GNU ld default linker script's own .init_array rule
	 * (`ld --verbose` prints it): the NNNNN suffix *is* an explicit
	 * init_priority ordering, so unlike .tdata.* / .tbss.* (grouped by
	 * name, order genuinely doesn't matter there), globbing these
	 * unsorted would link fine but silently run higher-priority
	 * constructors after lower-priority ones whenever more than one
	 * library/TU-provided entry exists at once.
	 */
	.init_array : {
		__init_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP(*(.init_array .ctors))
		__init_array_end = .;
	}

	. = ALIGN(16);

	.fini_array : {
		__fini_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
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
