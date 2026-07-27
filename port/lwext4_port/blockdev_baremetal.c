// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// blockdev_baremetal.c -- lwext4 block device backed directly by
// libBareMetal's raw sector I/O (b_nvs_read/b_nvs_write). The kernel's
// NVS calls already move data in fixed 4096-byte units addressed by a
// flat "starting sector" number (see BareMetal/syscalls/nvs.asm), which
// is exactly lwext4's block device contract (blk_id/blk_cnt of
// bdif->ph_bsize-sized blocks) -- so bread()/bwrite() below are a
// direct passthrough, no translation needed.
// =============================================================================

#include <ext4_config.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>

#include "libBareMetal.h"
#include "blockdev_baremetal.h"

#define BAREMETAL_BLK_DRIVE 0
#define BAREMETAL_BLK_BSIZE 4096

// There's no b_system() call to ask the kernel how big the backing
// disk actually is (see FIRECRACKER.md's drive setup -- disk.img's
// size is a host-side concern this port never sees), and ext4_mount()
// needs *some* upper bound up front to size bdev->part_size before it
// has even read the superblock that would tell it the real figure.
// This is a generous ceiling (2 GiB), not a claim about the real disk:
// lwext4 only bounds-checks reads/writes against it, it never reads
// or writes this many sectors unless the mounted filesystem actually
// has that many blocks in use. Raise it if a larger EXT2 image is
// ever used.
#define BAREMETAL_BLK_COUNT ((2ULL * 1024 * 1024 * 1024) / BAREMETAL_BLK_BSIZE)

static int bd_open(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return EOK;
}

static int bd_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
	(void)bdev;
	if (!blk_cnt)
		return EOK;
	return b_nvs_read(buf, blk_id, blk_cnt, BAREMETAL_BLK_DRIVE) == blk_cnt ? EOK : EIO;
}

static int bd_bwrite(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt)
{
	(void)bdev;
	if (!blk_cnt)
		return EOK;
	return b_nvs_write((void *)buf, blk_id, blk_cnt, BAREMETAL_BLK_DRIVE) == blk_cnt ? EOK : EIO;
}

static int bd_close(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return EOK;
}

EXT4_BLOCKDEV_STATIC_INSTANCE(baremetal_blockdev, BAREMETAL_BLK_BSIZE, BAREMETAL_BLK_COUNT,
			      bd_open, bd_bread, bd_bwrite, bd_close, 0, 0);

struct ext4_blockdev *baremetal_blockdev_get(void)
{
	return &baremetal_blockdev;
}

// =============================================================================
// EOF
