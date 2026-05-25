#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "vfs.h"

#define MAX_BLOCK_DEVICES 16

void blockdev_init();
int blockdev_register(block_device_t *device);
int blockdev_count();
block_device_t *blockdev_get(int index);

#endif
