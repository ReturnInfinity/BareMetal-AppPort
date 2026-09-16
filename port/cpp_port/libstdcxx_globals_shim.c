/*
 * Two plain global symbols libstdc++.a's precompiled object code
 * references directly, normally supplied by glibc's own runtime/
 * crtstuff rather than by anything an app or musl itself provides:
 *
 * __dso_handle: crtbegin.o's usual job (self-referential `void
 * *__dso_handle = &__dso_handle;`, a unique-enough "handle" for the
 * current module) -- passed to __cxa_atexit() so destructors could in
 * principle be run selectively per shared-object on dlclose(). This
 * port has no dlclose()/module-unload concept at all (one flat image,
 * one lifetime), so any single consistent address is correct; the
 * usual self-referential form is reused rather than inventing a new
 * convention.
 *
 * __libc_single_threaded: a real glibc >= 2.32 global libstdc++
 * checks (ios_base::Init, std::locale's refcounting, ...) to skip
 * atomic-refcount overhead when only one thread is running. This port
 * *does* have real threads (thread_shim.c/pthread_create), and unlike
 * glibc's own pthread_create -- which flips this to false the moment
 * a second thread is spawned -- nothing here updates it dynamically.
 * Hard-coding it false (never single-threaded, always take the safe
 * atomic path) is therefore the only value that's correct in every
 * case; hard-coding true would silently corrupt refcounts the first
 * time an app actually uses more than one thread.
 */

void *__dso_handle = &__dso_handle;
unsigned char __libc_single_threaded = 0;
