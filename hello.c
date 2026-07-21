// helloc.c -- Output a 'hello world' message via musl's printf,
// proving the musl -> posix_shim -> libBareMetal path works end to end.
// build with build-app.sh

#include <stdio.h>

int main()
{
	printf("Hello, World!\n");
	return 0;
}
