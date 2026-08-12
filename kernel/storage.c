#include "storage.h"
#include "ata.h"
#include "uhci.h"
#include "blockdev.h"
#include "partition.h"
#include "vfs.h"
#include "video.h"

void storage_init() {
    blockdev_init();

    /* USB Mass Storage Etapa 0+1: controller detection and port reset only --
     * no block_device_t yet (needs enumeration + Bulk-Only Transport first,
     * see ROADMAP.md). klog the outcome so it's visible without the `usb`
     * shell command. */
    if (uhci_init() == 0) {
        klog(uhci_status_string());
        /* Read-only (INQUIRY/TEST UNIT READY/READ CAPACITY/READ(10) LBA 0)
         * diagnostic for Etapa 3 Bulk-Only Transport + SCSI, logged the same
         * way as the enumeration status above -- safe to run unconditionally
         * even with no mass-storage device attached (uhci_msd_test_string
         * just reports that case in one line). The WRITE(10) roundtrip
         * variant stays manual-only (`usb msdwritetest`) since it actually
         * writes to whatever is attached. */
        klog(uhci_msd_test_string(0));

        /* Etapa 4: wrap the mass-storage device (if any) as a
         * block_device_t, same shape as ata_primary_master() below, so
         * partition_scan_mbr/vfs_mount pick it up with zero FAT32/ext2
         * changes. */
        if (uhci_msd_init() == 0) {
            block_device_t *usb_disk = uhci_msd_primary();
            if (usb_disk && blockdev_register(usb_disk) == 0) {
                klog("Storage: USB mass-storage device detected.");
                int usb_partitions = partition_scan_mbr(usb_disk);
                if (usb_partitions > 0) {
                    klog("Storage: MBR/GPT partitions detected on USB device.");
                } else {
                    klog("Storage: no MBR/GPT partitions found on USB device.");
                }
            }
        }
    } else {
        klog("USB: no UHCI controller found.\n");
    }

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
