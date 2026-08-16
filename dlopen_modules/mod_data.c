// mod_data.c -- exercises R_X86_64_RELATIVE relocations: every entry in
// names[] is a pointer into this module's own .rodata, so the linker
// can't fill it in at link time (the module's load address isn't known
// until dlfcn_shim.c's dlopen() picks one at runtime) -- each slot
// becomes a "bias + addend" relocation dlopen() has to apply by hand.
// get_name()/name_count() are plain functions, exercised the same way
// mod_extern.c's are, so a failure here isolates to the RELATIVE path
// specifically rather than function resolution in general.
//
// Build with ./build-module.sh dlopen_modules/mod_data.c -o build/mod_data.so

static const char *names[] = {
	"alpha", "bravo", "charlie", "delta",
};

const char *get_name(int i)
{
	if (i < 0 || i >= 4)
		return 0;
	return names[i];
}

int name_count(void)
{
	return 4;
}
