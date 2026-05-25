#include "storage.h"
#include "ata.h"
#include "blockdev.h"
#include "partition.h"
#include "vfs.h"
#include "video.h"

void storage_init() {
    blockdev_init();

    if (ata_init() == 0) {
        block_device_t *disk = ata_primary_master();
        if (disk && blockdev_register(disk) == 0) {
            klog("Storage: ATA primary master detected.");
            int partitions = partition_scan_mbr(disk);
            if (partitions > 0) {
                klog("Storage: MBR/GPT partitions detected.");
            } else {
                klog("Storage: no MBR/GPT partitions found.");
            }
        }
    } else {
        klog("Storage: no ATA primary master detected.");
    }

    for (int i = 0; i < blockdev_count(); i++) {
        block_device_t *device = blockdev_get(i);
        const char *fs_name = 0;
        vfs_node_t *root = vfs_mount(device, &fs_name);
        if (root && fs_name) {
            klog("Storage: filesystem mounted read-only.");
            klog(fs_name);
        }
    }
}
