// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// ext4_config.h -- lwext4 compile-time configuration for this port.
// Reached via lwext4's own include/ext4_config.h, which does
// "#include generated/ext4_config.h" (quote-form) when
// CONFIG_USE_DEFAULT_CFG=0 (see build-app.sh) -- so this file's path
// (port/lwext4_port/generated/ext4_config.h, found via the
// port/lwext4_port -I entry) is dictated by lwext4 itself, not chosen
// here. Vendored lwext4 has no include/generated/ directory of its
// own, so there's no risk of this losing to a same-named file the way
// mbedTLS's config does (see MBEDTLS_CFLAGS in build-app.sh).
//
// This file is included from inside ext4_config.h *before* that file
// defines F_SET_EXT2/F_SET_EXT3/F_SET_EXT4, so CONFIG_EXT_FEATURE_SET_LVL
// below has to use the raw value (2 == F_SET_EXT2), not the macro name.
// =============================================================================

#ifndef _BAREMETAL_EXT4_CONFIG_H
#define _BAREMETAL_EXT4_CONFIG_H

// Filesystem is EXT2 only -- no journal, no extents, both ext3/4-only
// on-disk features.
#define CONFIG_EXT_FEATURE_SET_LVL 2

#define CONFIG_JOURNALING_ENABLE 0
#define CONFIG_EXTENTS_ENABLE 0

// Left on despite EXT2 not needing it: ext4.c's ext4_{set,get,list,
// remove}xattr() wrappers call straight into ext4_xattr.c's
// implementation with no CONFIG_XATTR_ENABLE guard of their own (only
// ext4_xattr.c's *own* internals are guarded) -- so disabling it
// leaves those wrappers referencing functions that were never
// compiled in, and the link fails.
#define CONFIG_XATTR_ENABLE 1

// Reuse musl's errno.h / fcntl.h / unistd.h codes and O_*/SEEK_*
// values instead of lwext4's own -- posix_shim.c already hands lwext4
// the flags musl's syscalls issue, unmodified.
#define CONFIG_HAVE_OWN_ERRNO 0
#define CONFIG_HAVE_OWN_OFLAGS 0

// No stdio/printf plumbing wired up for lwext4's own debug tracing
// (see net_glue.c's separate BAREMETAL_DEBUG for this port's own
// diagnostics); disabling both also drops the printf-based
// ext4_assert() body down to a no-op instead of an infinite loop.
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0

// Standard malloc/free (musl's, backed by posix_shim.c's brk/mmap
// arena) rather than a user-supplied allocator.
#define CONFIG_USE_USER_MALLOC 0

#define CONFIG_BLOCK_DEV_ENABLE_STATS 0
#define CONFIG_BLOCK_DEV_CACHE_SIZE 8

#define CONFIG_EXT4_MAX_BLOCKDEV_NAME 32
#define CONFIG_EXT4_BLOCKDEVS_COUNT 1
#define CONFIG_EXT4_MAX_MP_NAME 32
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 1

#endif

// =============================================================================
// EOF
