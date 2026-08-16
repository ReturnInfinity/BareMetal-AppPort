// mod_ctor.c -- exercises DT_INIT_ARRAY: gcc emits the constructor as a
// RELATIVE-relocated function pointer in .init_array, so this only
// passes if dlopen() applies .rela.dyn to that array *before* walking
// it (see dlopen()'s ordering: apply_relocs() then init_array). If
// ctor_ran is still 0 by the time the caller checks it (dlsym'd and
// read *before* calling any function in this module), either the
// pointer never got relocated or init_array never ran at all.
//
// Build with ./build-module.sh dlopen_modules/mod_ctor.c -o build/mod_ctor.so

static int ctor_ran = 0;

__attribute__((constructor))
static void on_load(void)
{
	ctor_ran = 1;
}

int ctor_flag(void)
{
	return ctor_ran;
}
