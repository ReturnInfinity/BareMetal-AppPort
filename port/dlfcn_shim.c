// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// dlfcn_shim.c -- dlopen()/dlsym()/dlclose()/dlerror() for BareMetal apps.
//
// musl is built here with --disable-shared (see musl_port/
// musl-1.2.6-config.mak), so libc.a only carries weak stub versions of
// these four (src/ldso/dlopen.c etc in the musl tree -- always fail).
// The strong definitions below win the link against those stubs, the same
// way ext4_shim.c/tls_shim.c/etc already layer real implementations under
// musl's own surface.
//
// What this loads: a real ET_DYN ELF64 shared object built by
// ../build-module.sh -- NOT the app pipeline, which produces
// -fno-pic -fno-pie -mcmodel=large flat binaries with no relocation
// info at all, incompatible with loading at a runtime-chosen address.
//
// The module is read whole off the ext4-mounted disk via plain
// open()/read()/close() (see sqlite_vfs.c for the same pattern), copied
// into a malloc()'d block, and its .rela.dyn/.rela.plt entries are
// processed by hand -- there's no ld.so here to do it. This works with
// zero extra plumbing to make the copy executable: posix_shim.c's heap
// (which malloc() draws from) is one flat RWX arena, same as the rest
// of the app's own image -- see that file's header comment.
//
// Symbol resolution for anything a module references but doesn't define
// itself goes through dl_exports[] below -- a curated table, not a
// search over the host's own symbols (build-app.sh's objcopy -O binary
// step throws the host's symtab away, and exposing every host symbol to
// a loaded module is more than a plugin-style loader like this needs).
// Extend that table to expose more of the host to your modules.
// =============================================================================

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "libBareMetal.h"

// Minimal hand-written declarations for the three CPython C-API entries
// dl_exports[] below needs the addresses of -- deliberately not
// #include <Python.h>: this file is compiled into *every* app
// (build-app.sh), Python or not, and Python.h/pyconfig.h assume a build
// setup (an -I onto Python's Include tree, PY_SSIZE_T_CLEAN, etc) a
// generic shim like this one shouldn't force onto unrelated apps. These
// just need to be ABI-compatible enough to take an address from --
// port/python_port/ itself (and any real extension module, e.g.
// ../pyexttest.c) is what actually calls through them against the real
// Python.h declarations.
struct _object;
typedef struct _object PyObject;
struct PyModuleDef;
extern int PyArg_ParseTuple(PyObject *args, const char *format, ...);
// PY_SSIZE_T_CLEAN (the recommended, increasingly default-assumed way to
// write an extension -- see ../pyexttest.c) makes PyArg_ParseTuple a
// macro for this real symbol instead, so both need exporting: which one
// a given module's calls actually resolve to at link time depends on
// whether that module itself defined PY_SSIZE_T_CLEAN before #include
// <Python.h>, not on anything dlfcn_shim.c controls.
extern int _PyArg_ParseTuple_SizeT(PyObject *args, const char *format, ...);
extern PyObject *PyModule_Create2(struct PyModuleDef *module, int module_api_version);
extern PyObject *PyLong_FromLong(long v);

// -----------------------------------------------------------------------
// Curated export table -- symbols a loaded module is allowed to bind
// against for anything it doesn't define itself. Add entries as modules
// need more of the host surface; a module referencing something not
// listed here fails dlopen() with that symbol's name in dlerror().
//
// Function vs. data exports are NOT interchangeable here. A function
// export's table entry is just the function's own address -- a call
// through the GOT loads that address and jumps straight to it. A *data*
// symbol (e.g. a Python C-extension module doing `extern PyObject
// *PyExc_ValueError` and reading it) instead needs the table entry to be
// the address *of* the variable -- (void *)&PyExc_ValueError, not
// (void *)PyExc_ValueError -- since compiled PIC code dereferences the
// GOT slot once to get the variable's address, then dereferences again
// to read its current value. Every entry below is a function; there are
// no data exports yet.
// -----------------------------------------------------------------------

struct dl_export {
	const char *name;
	void *addr;
};

static const struct dl_export dl_exports[] = {
	{ "printf",  (void *)printf },
	{ "malloc",  (void *)malloc },
	{ "free",    (void *)free },
	{ "realloc", (void *)realloc },
	{ "calloc",  (void *)calloc },
	{ "memcpy",  (void *)memcpy },
	{ "memset",  (void *)memset },
	{ "memmove", (void *)memmove },
	{ "memcmp",  (void *)memcmp },
	{ "strlen",  (void *)strlen },
	{ "strcmp",  (void *)strcmp },
	{ "strncmp", (void *)strncmp },
	{ "strcpy",  (void *)strcpy },
	{ "strcat",  (void *)strcat },
	{ "b_output", (void *)b_output },
	// CPython C-API entries -- enough for a hand-written extension
	// module (see ../pyexttest.c) to prove Python/dynload_shlib.c's
	// dlopen()/dlsym() path end-to-end. All three are already real
	// symbols in the linked python.app binary (Python/getargs.c,
	// Python/modsupport.c, Objects/longobject.c are all in setup.sh's
	// PYTHON_SRCS). Extend this as more extensions need more of the
	// C API -- see this table's header comment for data vs. function
	// exports before adding a PyExc_*/PyType_Type-style symbol.
	{ "PyArg_ParseTuple", (void *)PyArg_ParseTuple },
	{ "_PyArg_ParseTuple_SizeT", (void *)_PyArg_ParseTuple_SizeT },
	{ "PyModule_Create2", (void *)PyModule_Create2 },
	{ "PyLong_FromLong",  (void *)PyLong_FromLong },
};

static void *dl_export_lookup(const char *name)
{
	for (size_t i = 0; i < sizeof(dl_exports) / sizeof(dl_exports[0]); i++) {
		if (strcmp(dl_exports[i].name, name) == 0)
			return dl_exports[i].addr;
	}
	return 0;
}

// -----------------------------------------------------------------------
// dlerror() state -- a single static buffer is enough here: apps run
// single-threaded per the rest of this port (see thread_shim.c), so
// there's no concurrent-dlopen() case to race on it.
// -----------------------------------------------------------------------

static char dl_error_buf[256];
static int dl_error_pending = 0;

// Every dlopen()/dlsym() failure path returns this, so callers can
// write `return dl_fail(...)` directly instead of setting the error and
// returning 0 as two separate statements.
static void *dl_fail(const char *msg)
{
	size_t n = strlen(msg);
	if (n >= sizeof(dl_error_buf))
		n = sizeof(dl_error_buf) - 1;
	memcpy(dl_error_buf, msg, n);
	dl_error_buf[n] = 0;
	dl_error_pending = 1;
	return 0;
}

// Same, for the common "<prefix>: <name>" case (missing file, missing
// symbol) -- avoids pulling in printf's format machinery for what's
// always just two strings joined together.
static void *dl_failf(const char *prefix, const char *name)
{
	size_t pn = strlen(prefix);
	if (pn > sizeof(dl_error_buf) - 3)
		pn = sizeof(dl_error_buf) - 3;
	size_t room = sizeof(dl_error_buf) - 1 - pn - 2;
	size_t nn = strlen(name);
	if (nn > room)
		nn = room;
	memcpy(dl_error_buf, prefix, pn);
	dl_error_buf[pn] = ':';
	dl_error_buf[pn + 1] = ' ';
	memcpy(dl_error_buf + pn + 2, name, nn);
	dl_error_buf[pn + 2 + nn] = 0;
	dl_error_pending = 1;
	return 0;
}

char *dlerror(void)
{
	if (!dl_error_pending)
		return 0;
	dl_error_pending = 0;
	return dl_error_buf;
}

// -----------------------------------------------------------------------
// Loaded-module handle
// -----------------------------------------------------------------------

struct dl_handle {
	char *block;			// malloc()'d backing store -- what dlclose() frees
	char *bias;			// block, rebased so bias+p_vaddr always lands inside it
	const Elf64_Sym *dynsym;
	const char *dynstr;
	size_t dynsym_count;		// from DT_HASH's nchain -- see dlopen() below
};

// Resolves relocs in one Elf64_Rela table (.rela.dyn or .rela.plt --
// same entry format, processed the same way). Every symbolic reference
// is resolved eagerly here rather than lazily through a PLT trampoline
// -- there's no runtime resolver to jump to on first call the way a
// real ld.so provides, so build-module.sh compiles modules -fno-plt to
// route external calls through the GOT directly, and everything is
// bound up front (equivalent to a real loader's -z now/BIND_NOW path).
static int apply_relocs(char *bias, const Elf64_Rela *relocs, size_t bytes,
			 const Elf64_Sym *dynsym, const char *dynstr)
{
	size_t count = bytes / sizeof(Elf64_Rela);

	for (size_t i = 0; i < count; i++) {
		const Elf64_Rela *r = &relocs[i];
		u64 type = ELF64_R_TYPE(r->r_info);
		u64 symidx = ELF64_R_SYM(r->r_info);
		u64 *target = (u64 *)(bias + r->r_offset);

		if (type == R_X86_64_RELATIVE) {
			*target = (u64)(bias + r->r_addend);
			continue;
		}

		if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT ||
		    type == R_X86_64_64) {
			const Elf64_Sym *sym = &dynsym[symidx];
			const char *name = dynstr + sym->st_name;
			void *resolved = (sym->st_shndx != SHN_UNDEF)
				? (void *)(bias + sym->st_value)
				: dl_export_lookup(name);
			if (!resolved) {
				dl_failf("dlopen: undefined symbol", name);
				return -1;
			}
			*target = (type == R_X86_64_64)
				? (u64)resolved + r->r_addend
				: (u64)resolved;
			continue;
		}

		dl_fail("dlopen: unsupported relocation type in module");
		return -1;
	}

	return 0;
}

void *dlopen(const char *path, int flags)
{
	(void)flags; // relocations are always processed eagerly -- see apply_relocs()

	if (!path)
		return dl_fail("dlopen: NULL path (dlopen(NULL, ...) -- handle to the host program -- is not supported here)");

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return dl_failf("dlopen: open failed", path);

	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return dl_failf("dlopen: fstat failed", path);
	}

	size_t fsize = (size_t)st.st_size;
	char *filebuf = malloc(fsize);
	if (!filebuf) {
		close(fd);
		return dl_fail("dlopen: out of memory reading module file");
	}

	size_t got = 0;
	while (got < fsize) {
		long n = read(fd, filebuf + got, fsize - got);
		if (n <= 0) {
			close(fd);
			free(filebuf);
			return dl_failf("dlopen: short read on", path);
		}
		got += (size_t)n;
	}
	close(fd);

	if (fsize < sizeof(Elf64_Ehdr)) {
		free(filebuf);
		return dl_fail("dlopen: file too small to be an ELF64 module");
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)filebuf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
	    eh->e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB) {
		free(filebuf);
		return dl_fail("dlopen: not a little-endian ELF64 file");
	}
	if (eh->e_type != ET_DYN || eh->e_machine != EM_X86_64) {
		free(filebuf);
		return dl_fail("dlopen: not an x86-64 ET_DYN shared object (build it with build-module.sh)");
	}

	Elf64_Phdr *phdrs = (Elf64_Phdr *)(filebuf + eh->e_phoff);
	int64_t min_vaddr = -1;
	int64_t max_vaddr = 0;
	Elf64_Phdr *dyn_phdr = 0;

	for (int i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = &phdrs[i];
		if (ph->p_type == PT_LOAD) {
			if (min_vaddr < 0 || (int64_t)ph->p_vaddr < min_vaddr)
				min_vaddr = (int64_t)ph->p_vaddr;
			int64_t end = (int64_t)(ph->p_vaddr + ph->p_memsz);
			if (end > max_vaddr)
				max_vaddr = end;
		} else if (ph->p_type == PT_DYNAMIC) {
			dyn_phdr = ph;
		}
	}
	if (min_vaddr < 0) {
		free(filebuf);
		return dl_fail("dlopen: module has no PT_LOAD segments");
	}
	if (!dyn_phdr) {
		free(filebuf);
		return dl_fail("dlopen: module has no PT_DYNAMIC segment");
	}

	size_t span = (size_t)(max_vaddr - min_vaddr);
	char *block = malloc(span);
	if (!block) {
		free(filebuf);
		return dl_fail("dlopen: out of memory loading module");
	}
	char *bias = block - min_vaddr;

	for (int i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = &phdrs[i];
		if (ph->p_type != PT_LOAD)
			continue;
		char *dst = bias + ph->p_vaddr;
		memcpy(dst, filebuf + ph->p_offset, ph->p_filesz);
		memset(dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
	}

	Elf64_Dyn *dyn = (Elf64_Dyn *)(bias + dyn_phdr->p_vaddr);
	const Elf64_Sym *dynsym = 0;
	const char *dynstr = 0;
	const Elf64_Rela *rela = 0; size_t relasz = 0;
	const Elf64_Rela *jmprel = 0; size_t pltrelsz = 0;
	const u64 *init_array = 0; size_t init_arraysz = 0;
	size_t dynsym_count = 0;

	for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
		switch (d->d_tag) {
		case DT_SYMTAB:      dynsym = (const Elf64_Sym *)(bias + d->d_un.d_ptr); break;
		case DT_STRTAB:      dynstr = (const char *)(bias + d->d_un.d_ptr); break;
		case DT_RELA:        rela = (const Elf64_Rela *)(bias + d->d_un.d_ptr); break;
		case DT_RELASZ:      relasz = (size_t)d->d_un.d_val; break;
		case DT_JMPREL:      jmprel = (const Elf64_Rela *)(bias + d->d_un.d_ptr); break;
		case DT_PLTRELSZ:    pltrelsz = (size_t)d->d_un.d_val; break;
		case DT_INIT_ARRAY:  init_array = (const u64 *)(bias + d->d_un.d_ptr); break;
		case DT_INIT_ARRAYSZ: init_arraysz = (size_t)d->d_un.d_val; break;
		case DT_HASH: {
			// hash[0] = nbucket, hash[1] = nchain; the ELF spec
			// guarantees nchain == the symbol table's entry
			// count. build-module.sh passes --hash-style=both
			// so this section always exists to read it from,
			// even though nothing here uses the hash itself.
			const Elf64_Word *hash = (const Elf64_Word *)(bias + d->d_un.d_ptr);
			dynsym_count = hash[1];
			break;
		}
		default: break;
		}
	}

	if (!dynsym || !dynstr || !dynsym_count) {
		free(block);
		free(filebuf);
		return dl_fail("dlopen: module missing .dynsym/.dynstr/.hash");
	}

	if (rela && relasz && apply_relocs(bias, rela, relasz, dynsym, dynstr) != 0) {
		free(block);
		free(filebuf);
		return 0; // apply_relocs() already set dlerror()
	}
	if (jmprel && pltrelsz && apply_relocs(bias, jmprel, pltrelsz, dynsym, dynstr) != 0) {
		free(block);
		free(filebuf);
		return 0;
	}

	if (init_array) {
		size_t n = init_arraysz / sizeof(u64);
		for (size_t i = 0; i < n; i++) {
			void (*ctor)(void) = (void (*)(void))init_array[i];
			if (ctor)
				ctor();
		}
	}

	free(filebuf); // module now lives entirely in `block`

	struct dl_handle *h = malloc(sizeof(struct dl_handle));
	if (!h) {
		free(block);
		return dl_fail("dlopen: out of memory allocating handle");
	}
	h->block = block;
	h->bias = bias;
	h->dynsym = dynsym;
	h->dynstr = dynstr;
	h->dynsym_count = dynsym_count;

	return h;
}

void *dlsym(void *handle, const char *name)
{
	struct dl_handle *h = handle;
	if (!h)
		return dl_fail("dlsym: NULL handle");

	for (size_t i = 0; i < h->dynsym_count; i++) {
		const Elf64_Sym *sym = &h->dynsym[i];
		if (sym->st_shndx == SHN_UNDEF || !sym->st_name)
			continue;
		if (strcmp(h->dynstr + sym->st_name, name) == 0)
			return h->bias + sym->st_value;
	}

	return dl_failf("dlsym: symbol not found", name);
}

int dlclose(void *handle)
{
	struct dl_handle *h = handle;
	if (!h)
		return 0;
	free(h->block);
	free(h);
	return 0;
}
