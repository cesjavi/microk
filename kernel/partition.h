#ifndef PARTITION_H
#define PARTITION_H

#include "vfs.h"

int partition_scan_mbr(block_device_t *disk);
int partition_scan_gpt(block_device_t *disk);

#endif
