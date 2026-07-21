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
	}

	. = ALIGN(16);

	.data : {
		*(.data .data.*)
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
	 */
	.init_array : {
		__init_array_start = .;
		*(.init_array .ctors)
		__init_array_end = .;
	}

	. = ALIGN(16);

	.fini_array : {
		__fini_array_start = .;
		*(.fini_array .dtors)
		__fini_array_end = .;
	}

	. = ALIGN(16);

	__bss_start = .;
	.bss : {
		*(.bss .bss.*)
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
