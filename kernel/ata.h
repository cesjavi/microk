#ifndef ATA_H
#define ATA_H

#include "vfs.h"

int ata_init();
block_device_t *ata_primary_master();

#endif
