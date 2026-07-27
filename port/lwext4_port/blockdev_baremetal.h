#ifndef _BLOCKDEV_BAREMETAL_H
#define _BLOCKDEV_BAREMETAL_H

struct ext4_blockdev;

// Returns the static lwext4 block device backed by b_nvs_read/
// b_nvs_write (see blockdev_baremetal.c). Used by ext4_shim.c to
// register/mount the EXT2 volume.
struct ext4_blockdev *baremetal_blockdev_get(void);

#endif
