// mod_undef.c -- deliberately references a symbol that will never be in
// dlfcn_shim.c's dl_exports[] table, so it links fine (ld doesn't
// require shared-object undefined symbols to resolve) but dlopen()
// must fail at relocation time with "undefined symbol:
// not_a_real_dl_export_symbol" in dlerror(). Exercises apply_relocs()'s
// failure path, not just its success path -- no other module here
// takes it.
//
// Build with ./build-module.sh dlopen_modules/mod_undef.c -o build/mod_undef.so

extern int not_a_real_dl_export_symbol(void);

int call_it(void)
{
	return not_a_real_dl_export_symbol();
}
