// Minimal dlopen() target -- build with:
//   ./build-module.sh modtest.c -o modtest.so
// then install it onto disk.img (e.g.
//   ./scripts/install-file.sh disk.img modtest.so /modules/modtest.so)
// so dltest.c's dlopen("/modules/modtest.so", 0) can find it at runtime.

#include <stdio.h>

int add(int a, int b)
{
	return a + b;
}

void greet(void)
{
	printf("Hello from a dlopen()'d module!\n");
}
