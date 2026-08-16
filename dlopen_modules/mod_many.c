// mod_many.c -- a module with many exported symbols, so dlsym()'s
// linear scan (dlfcn_shim.c's dlsym() walks h->dynsym_count entries
// looking for a name match) gets exercised at more than the one- or
// two-symbol scale every other module here uses, including a symbol
// deliberately placed last (fn19) so a scan that stops early wouldn't
// find it. fn_call_chain() also exercises one exported function
// calling another *within the same module* -- a direct PC-relative
// call, not a GOT/dl_exports round trip -- so that path is exercised
// somewhere too.
//
// Build with ./build-module.sh dlopen_modules/mod_many.c -o build/mod_many.so

#define FN(n) int fn##n(int x) { return x + n; }

FN(0) FN(1) FN(2) FN(3) FN(4) FN(5) FN(6) FN(7) FN(8) FN(9)
FN(10) FN(11) FN(12) FN(13) FN(14) FN(15) FN(16) FN(17) FN(18)

int fn19(int x)
{
	return x + 19;
}

int fn_call_chain(int x)
{
	return fn19(fn0(x));
}
