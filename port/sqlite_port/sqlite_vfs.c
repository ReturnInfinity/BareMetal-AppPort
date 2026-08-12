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
// calls the rest of this port's apps use (posix_shim.c -> ext4_shim.c)
// -- not a port of os_unix.c's own logic. That's deliberate, the same
// choice this port already made for TLS/DNS/sockets (tls_shim.c,
// dns_shim.c, net_shim.c) rather than one big shim trying to emulate
// everything a general-purpose POSIX layer would: os_unix.c leans on
// fcntl() advisory locking and mmap()-backed WAL shared memory, neither
// of which this port has a real implementation of, and coaxing correct
// behavior out of stubs for each would be more fragile than just not
// calling them.
//
// What that buys, given this port's model (exactly one process,
// exactly one thread, ever -- see OPENISSUES.md's "Process model"):
//
//   - Locking (ext2Lock/ext2Unlock/ext2CheckReservedLock) is a pure
//     no-op that always reports success/uncontended: there is never
//     a second connection for a lock to conflict with.
//   - Syncing (ext2Sync) is a pure no-op: lwext4's own bcache only ever
//     runs in write-back mode transiently, inside a single ext4.c call
//     (ext4_fwrite(), ext4_fclose(), ...) -- it flips back to
//     write-through, flushing every dirty block to b_nvs_write(),
//     before that call returns. So there's never a dirty block left
//     sitting in the cache by the time control comes back to this VFS
//     for a sync to flush.
//   - No WAL, no mmap -- see sqlite_baremetal_config.h's
//     SQLITE_OMIT_WAL/SQLITE_MAX_MMAP_SIZE comments. ext2_io_methods
//     below is iVersion 1 (no xShm-prefixed or xFetch/xUnfetch slots
//     at all) to match, on top of the compile-time omission.
//   - Randomness (ext2Randomness) is RDRAND, the same hardware source
//     port/mbedtls_port/entropy_hardware_poll.c uses for mbedTLS (see
//     that file's header) -- duplicated here in miniature rather than
//     shared, to keep sqlite_port/ independent of mbedtls_port/ the
//     same way curl_port/lwip_port/mbedtls_port don't reference each
//     other either.
//   - Time (ext2CurrentTime/ext2CurrentTimeInt64) goes through the
//     same clock_gettime(CLOCK_REALTIME) posix_shim.c already backs
//     with b_system(WALLCLOCK) -- whole seconds only, no sub-second
//     component (see posix_shim.c's "Time" section).
//   - Sleeping (ext2Sleep) spin-waits on CLOCK_MONOTONIC instead of
//     blocking: there's no nanosleep()/clock_nanosleep() on this port
//     (OPENISSUES.md). In practice this is dead code here anyway --
//     it's only ever reached from SQLite's busy-handler retry loop
//     after a lock request fails, and locks above never fail.
//
// Temp files (SQLITE_TEMP_STORE=3, set in sqlite_baremetal_config.h)
// keeps ordinary TEMP tables/indices and the transient sorters/
// statement journals ORDER BY, GROUP BY, CREATE INDEX etc. use
// in-memory unconditionally, so ext2Open()'s zName==0 path (SQLite's
// "invent your own filename" convention for a real temp file) is only
// ever reached for a multi-database (ATTACH) transaction's master
// journal -- rare, but handled below (a name built from ext2Randomness
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

// A generous buffer cap for the strings this file builds/copies
// (including the "-journal"/"etilqs_..." suffixes SQLite's own
// pager.c appends before ever calling into this file) -- not a claim
// about EXT2's own name/path limits (255 bytes per component; see
// ext4_shim.c's EXT4_SHIM_PATH_MAX for this port's own resolved-path
// buffer cap).
#define MAXPATHNAME 512

typedef struct Ext2File {
	sqlite3_file base;
	int fd;
	int delete_on_close;
	char name[MAXPATHNAME];
} Ext2File;

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

static int ext2Close(sqlite3_file *pFile)
{
	Ext2File *p = (Ext2File *)pFile;

	close(p->fd);
	if (p->delete_on_close)
		unlink(p->name);

	return SQLITE_OK;
}

// SQLite requires short reads (reading at/past EOF) to zero-fill the
// unread tail and report SQLITE_IOERR_SHORT_READ rather than treating
// it as a hard error -- the page cache relies on this to tell "file
// just hasn't grown to this page yet" apart from a real I/O failure.
static int ext2Read(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst)
{
	Ext2File *p = (Ext2File *)pFile;

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

static int ext2Write(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite3_int64 iOfst)
{
	Ext2File *p = (Ext2File *)pFile;

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

static int ext2Truncate(sqlite3_file *pFile, sqlite3_int64 size)
{
	Ext2File *p = (Ext2File *)pFile;

	if (ftruncate(p->fd, (long)size) != 0)
		return SQLITE_IOERR_TRUNCATE;

	return SQLITE_OK;
}

static int ext2Sync(sqlite3_file *pFile, int flags)
{
	(void)pFile; (void)flags;
	return SQLITE_OK; // see this file's header
}

static int ext2FileSize(sqlite3_file *pFile, sqlite3_int64 *pSize)
{
	Ext2File *p = (Ext2File *)pFile;
	struct stat st;

	if (fstat(p->fd, &st) != 0)
		return SQLITE_IOERR_FSTAT;

	*pSize = st.st_size;
	return SQLITE_OK;
}

static int ext2Lock(sqlite3_file *pFile, int eLock)
{
	(void)pFile; (void)eLock;
	return SQLITE_OK; // see this file's header
}

static int ext2Unlock(sqlite3_file *pFile, int eLock)
{
	(void)pFile; (void)eLock;
	return SQLITE_OK;
}

static int ext2CheckReservedLock(sqlite3_file *pFile, int *pResOut)
{
	(void)pFile;
	*pResOut = 0; // never reserved -- no other connection could hold it
	return SQLITE_OK;
}

static int ext2FileControl(sqlite3_file *pFile, int op, void *pArg)
{
	(void)pFile; (void)op; (void)pArg;
	return SQLITE_NOTFOUND; // no VFS-specific opcodes implemented
}

static int ext2SectorSize(sqlite3_file *pFile)
{
	(void)pFile;
	return 4096; // BAREMETAL_BLK_BSIZE, see port/lwext4_port/blockdev_baremetal.c
}

static int ext2DeviceCharacteristics(sqlite3_file *pFile)
{
	(void)pFile;
	return 0; // no special atomic-write/safe-append guarantees claimed
}

static const sqlite3_io_methods ext2_io_methods = {
	1, // iVersion -- see this file's header
	ext2Close,
	ext2Read,
	ext2Write,
	ext2Truncate,
	ext2Sync,
	ext2FileSize,
	ext2Lock,
	ext2Unlock,
	ext2CheckReservedLock,
	ext2FileControl,
	ext2SectorSize,
	ext2DeviceCharacteristics
};

// -----------------------------------------------------------------------
// sqlite3_vfs -- top-level (not-yet-open-file) operations.
// -----------------------------------------------------------------------

static int ext2Open(sqlite3_vfs *pVfs, const char *zName, sqlite3_file *pFile, int flags, int *pOutFlags)
{
	(void)pVfs;
	Ext2File *p = (Ext2File *)pFile;
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

	p->base.pMethods = &ext2_io_methods;
	p->fd = fd;
	p->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;

	if (pOutFlags)
		*pOutFlags = flags;

	return SQLITE_OK;
}

static int ext2Delete(sqlite3_vfs *pVfs, const char *zPath, int dirSync)
{
	(void)pVfs; (void)dirSync;
	unlink(zPath); // missing is fine -- see sqlite3_vfs.xDelete's contract
	return SQLITE_OK;
}

// This port has no permission enforcement at all (see ext4_shim.c's
// header and OPENISSUES.md's "EXT2 file I/O" section -- a regular
// file's mode always reports as 0644 regardless of what's really on
// the inode) -- existence is the only thing to check, same answer for
// SQLITE_ACCESS_EXISTS/READWRITE/READ.
static int ext2Access(sqlite3_vfs *pVfs, const char *zPath, int flags, int *pResOut)
{
	(void)pVfs; (void)flags;
	struct stat st;

	*pResOut = (stat(zPath, &st) == 0);
	return SQLITE_OK;
}

// zPath is handed back unresolved rather than actually made absolute:
// ext4_shim.c's own open()/unlink()/stat() etc. already resolve a
// relative path against its cwd (or reject it, for a dirfd-relative
// call this VFS never makes) themselves, so there's nothing this
// needs to do beyond satisfying the xFullPathname contract that
// *something* comes back in zOut.
static int ext2FullPathname(sqlite3_vfs *pVfs, const char *zPath, int nOut, char *zOut)
{
	(void)pVfs;
	snprintf(zOut, (size_t)nOut, "%s", zPath);
	return SQLITE_OK;
}

// No dynamic linking on this port at all (OPENISSUES.md's "General"
// section) -- unreachable in practice with SQLITE_OMIT_LOAD_EXTENSION
// set (sqlite_baremetal_config.h), stubbed here anyway since the VFS
// struct's shape still has these slots.
static void *ext2DlOpen(sqlite3_vfs *pVfs, const char *zFilename)
{
	(void)pVfs; (void)zFilename;
	return 0;
}

static void ext2DlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg)
{
	(void)pVfs;
	if (nByte > 0)
		zErrMsg[0] = 0;
}

static void (*ext2DlSym(sqlite3_vfs *pVfs, void *pH, const char *zSym))(void)
{
	(void)pVfs; (void)pH; (void)zSym;
	return 0;
}

static void ext2DlClose(sqlite3_vfs *pVfs, void *pHandle)
{
	(void)pVfs; (void)pHandle;
}

static int ext2Randomness(sqlite3_vfs *pVfs, int nBuf, char *zBuf)
{
	(void)pVfs;
	rdrand_bytes(zBuf, (size_t)nBuf);
	return nBuf;
}

static int ext2Sleep(sqlite3_vfs *pVfs, int microseconds)
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
static int ext2CurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *pNow)
{
	(void)pVfs;
	static const sqlite3_int64 unix_epoch = 24405875LL * (sqlite3_int64)8640000;
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	*pNow = unix_epoch + (sqlite3_int64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	return SQLITE_OK;
}

static int ext2CurrentTime(sqlite3_vfs *pVfs, double *pNow)
{
	sqlite3_int64 i = 0;

	ext2CurrentTimeInt64(pVfs, &i);
	*pNow = i / 86400000.0;

	return SQLITE_OK;
}

static int ext2GetLastError(sqlite3_vfs *pVfs, int nBuf, char *zBuf)
{
	(void)pVfs;
	if (nBuf > 0)
		zBuf[0] = 0;
	return 0;
}

static sqlite3_vfs ext2_vfs = {
	2,                 // iVersion -- has xCurrentTimeInt64
	sizeof(Ext2File),  // szOsFile
	MAXPATHNAME,       // mxPathname
	0,                 // pNext -- filled in by sqlite3_vfs_register()
	"ext2",            // zName
	0,                 // pAppData
	ext2Open,
	ext2Delete,
	ext2Access,
	ext2FullPathname,
	ext2DlOpen,
	ext2DlError,
	ext2DlSym,
	ext2DlClose,
	ext2Randomness,
	ext2Sleep,
	ext2CurrentTime,
	ext2GetLastError,
	ext2CurrentTimeInt64, // iVersion 2
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
	return sqlite3_vfs_register(&ext2_vfs, 1);
}

int sqlite3_os_end(void)
{
	return SQLITE_OK;
}

// =============================================================================
// EOF
