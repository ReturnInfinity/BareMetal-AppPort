// Exercises port/dlfcn_shim.c's dlopen()/dlsym()/dlclose()/dlerror().
// Build modtest.c with ./build-module.sh and install it at
// /modules/modtest.so on disk.img first -- see that file's own comment.

#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
	void *h = dlopen("/modules/modtest.so", 0);
	if (!h) {
		printf("dlopen failed: %s\n", dlerror());
		return 1;
	}

	int (*add)(int, int) = dlsym(h, "add");
	if (!add) {
		printf("dlsym(add) failed: %s\n", dlerror());
		return 1;
	}
	printf("add(1, 2) = %d\n", add(1, 2));

	void (*greet)(void) = dlsym(h, "greet");
	if (!greet) {
		printf("dlsym(greet) failed: %s\n", dlerror());
		return 1;
	}
	greet();

	if (dlsym(h, "no_such_symbol") != 0) {
		printf("dlsym unexpectedly found a nonexistent symbol\n");
		return 1;
	}
	printf("dlsym(no_such_symbol) correctly failed: %s\n", dlerror());

	dlclose(h);

	if (dlopen("/modules/does_not_exist.so", 0) != 0) {
		printf("dlopen unexpectedly succeeded on a missing file\n");
		return 1;
	}
	printf("dlopen(missing file) correctly failed: %s\n", dlerror());

	printf("dltest: all checks passed\n");
	return 0;
}
