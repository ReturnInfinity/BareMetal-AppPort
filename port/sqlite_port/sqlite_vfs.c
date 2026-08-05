// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// sqlite_vfs.c -- a minimal sqlite3_vfs for this port, and the
// sqlite3_os_init()/sqlite3_os_end() pair SQLITE_OS_OTHER=1 (see
// sqlite_baremetal_config.h) requires the application to supply in
// place of SQLite's own os_unix.c.
//
// Every method here is a thin, direct wrapper over the same
// open/read/write/lseek/close/unlink/ftruncate/stat/clock_gettime
// calls the rest of this port's apps use (posix_shim.c -> bmfs.c) --
// not a port of os_unix.c's own logic. That's deliberate, the same
// choice this port already made for TLS/DNS/sockets (tls_shim.c,
// dns_shim.c, net_shim.c) rather than one big shim trying to emulate
// everything a general-purpose POSIX layer would: os_unix.c leans on
// fcntl() advisory locking, mmap()-backed WAL shared memory,
// /dev/urandom, and getcwd(), none of which this port has a real
// implementation of, and coaxing correct behavior out of stubs for
// each would be more fragile than just not calling them.
//
// What that buys, given this port's model (exactly one process,
// exactly one thread, ever -- see OPENISSUES.md's "Process model"):
//
//   - Locking (bmfsLock/bmfsUnlock/bmfsCheckReservedLock) is a pure
//     no-op that always reports success/uncontended: there is never
//     a second connection for a lock to conflict with.
//   - Syncing (bmfsSync) is a pure no-op: bmfs_write() (port/bmfs.c)
//     already issues its b_nvs_write() disk I/O synchronously, one
//     sector at a time, as data is written -- there's no write-back
//     cache in this port for a sync to flush.
//   - No WAL, no mmap -- see sqlite_baremetal_config.h's
//     SQLITE_OMIT_WAL/SQLITE_MAX_MMAP_SIZE comments. bmfs_io_methods
//     below is iVersion 1 (no xShm-prefixed or xFetch/xUnfetch slots
//     at all) to match, on top of the compile-time omission.
//   - Randomness (bmfsRandomness) is RDRAND, the same hardware source
//     port/mbedtls_port/entropy_hardware_poll.c uses for mbedTLS (see
//     that file's header) -- duplicated here in miniature rather than
//     shared, to keep sqlite_port/ independent of mbedtls_port/ the
//     same way curl_port/lwip_port/mbedtls_port don't reference each
//     other either.
//   - Time (bmfsCurrentTime/bmfsCurrentTimeInt64) goes through the
//     same clock_gettime(CLOCK_REALTIME) posix_shim.c already backs
//     with b_system(WALLCLOCK) -- whole seconds only, no sub-second
//     component (see posix_shim.c's "Time" section).
//   - Sleeping (bmfsSleep) spin-waits on CLOCK_MONOTONIC instead of
//     blocking: there's no nanosleep()/clock_nanosleep() on this port
//     (OPENISSUES.md). In practice this is dead code here anyway --
//     it's only ever reached from SQLite's busy-handler retry loop
//     after a lock request fails, and locks above never fail.
//
// Temp files (SQLITE_TEMP_STORE=3, set in sqlite_baremetal_config.h)
// keeps ordinary TEMP tables/indices and the transient sorters/
// statement journals ORDER BY, GROUP BY, CREATE INDEX etc. use
// in-memory unconditionally, so bmfsOpen()'s zName==0 path (SQLite's
// "invent your own filename" convention for a real temp file) is only
// ever reached for a multi-database (ATTACH) transaction's master
// journal -- rare, but handled below (a name built from bmfsRandomness
// rather than left to fail), not assumed away.
// =============================================================================

#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>

#include "sqlite3.h"

// BMFS filenames are at most 31 bytes + NUL (see port/bmfs.c) -- any
// name this VFS builds (including the "-journal"/"etilqs_..." suffixes
// SQLite's own pager.c appends before ever calling into this file)
// that runs past that will simply fail to open with -ENOENT, surfaced
// below as SQLITE_CANTOPEN. This is sized as a generous buffer cap for
// the strings this file builds/copies, not a claim that BMFS itself
// can hold paths this long.
#define MAXPATHNAME 512

typedef struct BmfsFile {
	sqlite3_file base;
	int fd;
	int delete_on_close;
	char name[MAXPATHNAME];
} BmfsFile;

// -----------------------------------------------------------------------
// Hardware randomness -- same RDRAND-with-RDTSC-fallback technique as
// port/mbedtls_port/entropy_hardware_poll.c (see that file's header
// for why: no /dev/urandom, no getrandom() syscall on this port).
// Duplicated rather than shared -- see this file's header.
// -----------------------------------------------------------------------

static int has_rdrand(void)
{
	unsigned int eax, ebx, ecx, edx;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
	return (ecx >> 30) & 1;
}

static void rdrand_bytes(void *out, size_t len)
{
	int rdrand_ok = has_rdrand();
	unsigned char *output = out;
	size_t n = 0;

	while (n < len) {
		unsigned long v = 0;
		int ok = 0;

		if (rdrand_ok) {
			for (int tries = 0; tries < 10 && !ok; tries++)
				__asm__ volatile ("rdrand %0\n\tsetc %b1" : "=r"(v), "=q"(ok) :: "cc");
		}

		if (!ok) {
			unsigned int lo, hi;
			__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
			v = ((unsigned long)hi << 32 | lo) ^ (unsigned long)output ^ (unsigned long)n;
		}

		size_t chunk = len - n < sizeof(v) ? len - n : sizeof(v);
		memcpy(output + n, &v, chunk);
		n += chunk;
	}
}

// -----------------------------------------------------------------------
// sqlite3_io_methods -- per-open-file operations. iVersion 1: no
// xShmMap/xShmLock/xShmBarrier/xShmUnmap/xFetch/xUnfetch slots exist
// in this struct shape at all, so nothing in SQLite core can reach for
// them regardless of what runtime state or pragma asks for WAL/mmap
// (see sqlite_baremetal_config.h).
// -----------------------------------------------------------------------

static int bmfsClose(sqlite3_file *pFile)
{
	BmfsFile *p = (BmfsFile *)pFile;

	close(p->fd);
	if (p->delete_on_close)
		unlink(p->name);

	return SQLITE_OK;
}

// SQLite requires short reads (reading at/past EOF) to zero-fill the
// unread tail and report SQLITE_IOERR_SHORT_READ rather than treating
// it as a hard error -- the page cache relies on this to tell "file
// just hasn't grown to this page yet" apart from a real I/O failure.
static int bmfsRead(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst)
{
	BmfsFile *p = (BmfsFile *)pFile;

	if (lseek(p->fd, (long)iOfst, SEEK_SET) < 0)
		return SQLITE_IOERR_READ;

	int got = 0;
	while (got < iAmt) {
		long n = read(p->fd, (char *)zBuf + got, (size_t)(iAmt - got));
		if (n < 0)
			return SQLITE_IOERR_READ;
		if (n == 0)
			break;
		got += (int)n;
	}

	if (got < iAmt) {
		memset((char *)zBuf + got, 0, (size_t)(iAmt - got));
		return SQLITE_IOERR_SHORT_READ;
	}

	return SQLITE_OK;
}

static int bmfsWrite(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite3_int64 iOfst)
{
	BmfsFile *p = (BmfsFile *)pFile;

	if (lseek(p->fd, (long)iOfst, SEEK_SET) < 0)
		return SQLITE_IOERR_WRITE;

	int done = 0;
	while (done < iAmt) {
		long n = write(p->fd, (const char *)zBuf + done, (size_t)(iAmt - done));
		if (n <= 0)
			return SQLITE_IOERR_WRITE;
		done += (int)n;
	}

	return SQLITE_OK;
}

static int bmfsTruncate(sqlite3_file *pFile, sqlite3_int64 size)
{
	BmfsFile *p = (BmfsFile *)pFile;

	if (ftruncate(p->fd, (long)size) != 0)
		return SQLITE_IOERR_TRUNCATE;

	return SQLITE_OK;
}

static int bmfsSync(sqlite3_file *pFile, int flags)
{
	(void)pFile; (void)flags;
	return SQLITE_OK; // see this file's header
}

static int bmfsFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize)
{
	BmfsFile *p = (BmfsFile *)pFile;
	struct stat st;

	if (fstat(p->fd, &st) != 0)
		return SQLITE_IOERR_FSTAT;

	*pSize = st.st_size;
	return SQLITE_OK;
}

static int bmfsLock(sqlite3_file *pFile, int eLock)
{
	(void)pFile; (void)eLock;
	return SQLITE_OK; // see this file's header
}

static int bmfsUnlock(sqlite3_file *pFile, int eLock)
{
	(void)pFile; (void)eLock;
	return SQLITE_OK;
}

static int bmfsCheckReservedLock(sqlite3_file *pFile, int *pResOut)
{
	(void)pFile;
	*pResOut = 0; // never reserved -- no other connection could hold it
	return SQLITE_OK;
}

static int bmfsFileControl(sqlite3_file *pFile, int op, void *pArg)
{
	(void)pFile; (void)op; (void)pArg;
	return SQLITE_NOTFOUND; // no VFS-specific opcodes implemented
}

static int bmfsSectorSize(sqlite3_file *pFile)
{
	(void)pFile;
	return 4096; // BMFS_SECTOR_BYTES, see port/bmfs.c
}

static int bmfsDeviceCharacteristics(sqlite3_file *pFile)
{
	(void)pFile;
	return 0; // no special atomic-write/safe-append guarantees claimed
}

static const sqlite3_io_methods bmfs_io_methods = {
	1, // iVersion -- see this file's header
	bmfsClose,
	bmfsRead,
	bmfsWrite,
	bmfsTruncate,
	bmfsSync,
	bmfsFileSize,
	bmfsLock,
	bmfsUnlock,
	bmfsCheckReservedLock,
	bmfsFileControl,
	bmfsSectorSize,
	bmfsDeviceCharacteristics
};

// -----------------------------------------------------------------------
// sqlite3_vfs -- top-level (not-yet-open-file) operations.
// -----------------------------------------------------------------------

static int bmfsOpen(sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile, int flags, int *pOutFlags)
{
	(void)pVfs;
	BmfsFile *p = (BmfsFile *)pFile;
	char tmp[32];

	memset(p, 0, sizeof(*p));

	if (!zName) {
		// SQLite wants a fresh, unique file it names itself -- see
		// this file's header for when this path is actually
		// reached (master journal only, given SQLITE_TEMP_STORE=3).
		unsigned long r;
		rdrand_bytes(&r, sizeof(r));
		snprintf(tmp, sizeof(tmp), "tmp%08lx", r & 0xFFFFFFFFUL);
		zName = tmp;
		flags |= SQLITE_OPEN_DELETEONCLOSE;
	}

	size_t len = strlen(zName);
	if (len >= sizeof(p->name))
		return SQLITE_CANTOPEN;
	memcpy(p->name, zName, len + 1);

	int oflags = 0;
	if (flags & SQLITE_OPEN_EXCLUSIVE)
		oflags |= O_EXCL;
	if (flags & SQLITE_OPEN_CREATE)
		oflags |= O_CREAT;
	oflags |= (flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR;

	int fd = open(zName, oflags, 0644);
	if (fd < 0)
		return SQLITE_CANTOPEN;

	p->base.pMethods = &bmfs_io_methods;
	p->fd = fd;
	p->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;

	if (pOutFlags)
		*pOutFlags = flags;

	return SQLITE_OK;
}

static int bmfsDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync)
{
	(void)pVfs; (void)dirSync;
	unlink(zPath); // missing is fine -- see sqlite3_vfs.xDelete's contract
	return SQLITE_OK;
}

// BMFS has no permission model at all (every file reports mode 0644
// regardless -- see bmfs.c) -- existence is the only thing to check,
// same answer for SQLITE_ACCESS_EXISTS/READWRITE/READ.
static int bmfsAccess(sqlite3_vfs *pVfs, const char *zPath, int flags, int *pResOut)
{
	(void)pVfs; (void)flags;
	struct stat st;

	*pResOut = (stat(zPath, &st) == 0);
	return SQLITE_OK;
}

// BMFS is a flat namespace (see bmfs.c) -- there's no cwd, no `..`,
// nothing for a "full" pathname to resolve beyond the name itself.
static int bmfsFullPathname(sqlite3_vfs *pVfs, const char *zPath, int nOut, char *zOut)
{
	(void)pVfs;
	snprintf(zOut, (size_t)nOut, "%s", zPath);
	return SQLITE_OK;
}

// No dynamic linking on this port at all (OPENISSUES.md's "General"
// section) -- unreachable in practice with SQLITE_OMIT_LOAD_EXTENSION
// set (sqlite_baremetal_config.h), stubbed here anyway since the VFS
// struct's shape still has these slots.
static void *bmfsDlOpen(sqlite3_vfs *pVfs, const char *zFilename)
{
	(void)pVfs; (void)zFilename;
	return 0;
}

static void bmfsDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg)
{
	(void)pVfs;
	if (nByte > 0)
		zErrMsg[0] = 0;
}

static void (*bmfsDlSym(sqlite3_vfs *pVfs, void *pH, const char *zSym))(void)
{
	(void)pVfs; (void)pH; (void)zSym;
	return 0;
}

static void bmfsDlClose(sqlite3_vfs *pVfs, void *pHandle)
{
	(void)pVfs; (void)pHandle;
}

static int bmfsRandomness(sqlite3_vfs *pVfs, int nBuf, char *zBuf)
{
	(void)pVfs;
	rdrand_bytes(zBuf, (size_t)nBuf);
	return nBuf;
}

static int bmfsSleep(sqlite3_vfs *pVfs, int microseconds)
{
	(void)pVfs;
	struct timespec start, now;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while ((now.tv_sec - start.tv_sec) * 1000000L + (now.tv_nsec - start.tv_nsec) / 1000L < microseconds);

	return microseconds;
}

// Julian-day-based epoch SQLite's own os_unix.c uses (the 24405875*
// 8640000 constant is the same one unixCurrentTimeInt64() computes
// from -- Julian day 0 relative to the Unix epoch, in milliseconds).
static int bmfsCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *pNow)
{
	(void)pVfs;
	static const sqlite3_int64 unix_epoch = 24405875LL * (sqlite3_int64)8640000;
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	*pNow = unix_epoch + (sqlite3_int64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	return SQLITE_OK;
}

static int bmfsCurrentTime(sqlite3_vfs *pVfs, double *pNow)
{
	sqlite3_int64 i = 0;

	bmfsCurrentTimeInt64(pVfs, &i);
	*pNow = i / 86400000.0;

	return SQLITE_OK;
}

static int bmfsGetLastError(sqlite3_vfs *pVfs, int nBuf, char *zBuf)
{
	(void)pVfs;
	if (nBuf > 0)
		zBuf[0] = 0;
	return 0;
}

static sqlite3_vfs bmfs_vfs = {
	2,                 // iVersion -- has xCurrentTimeInt64
	sizeof(BmfsFile),  // szOsFile
	MAXPATHNAME,       // mxPathname
	0,                 // pNext -- filled in by sqlite3_vfs_register()
	"bmfs",            // zName
	0,                 // pAppData
	bmfsOpen,
	bmfsDelete,
	bmfsAccess,
	bmfsFullPathname,
	bmfsDlOpen,
	bmfsDlError,
	bmfsDlSym,
	bmfsDlClose,
	bmfsRandomness,
	bmfsSleep,
	bmfsCurrentTime,
	bmfsGetLastError,
	bmfsCurrentTimeInt64, // iVersion 2
	0,                 // xSetSystemCall  -- iVersion 3 only
	0,                 // xGetSystemCall  -- iVersion 3 only
	0,                 // xNextSystemCall -- iVersion 3 only
};

// Called automatically by sqlite3_initialize(), itself called lazily
// by the first sqlite3_open()/sqlite3_open_v2() an app makes (unless
// SQLITE_OMIT_AUTOINIT is set, which sqlite_baremetal_config.h
// deliberately leaves unset) -- no explicit setup call is needed from
// application code.
int sqlite3_os_init(void)
{
	return sqlite3_vfs_register(&bmfs_vfs, 1);
}

int sqlite3_os_end(void)
{
	return SQLITE_OK;
}

// =============================================================================
// EOF
