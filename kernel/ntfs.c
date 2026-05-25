#include "ntfs.h"
#include "kheap.h"
#include <string.h>

#define NTFS_BOOT_SECTOR_SIZE 512

typedef struct {
    uint8_t jump[3];
    char oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint8_t reserved0[7];
    uint8_t media_descriptor;
    uint8_t reserved1[18];
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mft_mirror_lcn;
    int8_t clusters_per_file_record;
    uint8_t reserved2[3];
    int8_t clusters_per_index_record;
} __attribute__((packed)) ntfs_boot_sector_t;

typedef struct {
    block_device_t *device;
    ntfs_boot_sector_t boot_sector;
    uint32_t cluster_size;
} ntfs_mount_t;

static int ntfs_signature_ok(ntfs_boot_sector_t *boot) {
    return boot->oem_id[0] == 'N' &&
           boot->oem_id[1] == 'T' &&
           boot->oem_id[2] == 'F' &&
           boot->oem_id[3] == 'S' &&
           boot->oem_id[4] == ' ' &&
           boot->oem_id[5] == ' ' &&
           boot->oem_id[6] == ' ' &&
           boot->oem_id[7] == ' ';
}

static int ntfs_read_boot_sector(block_device_t *device, ntfs_boot_sector_t *out) {
    if (vfs_read_device(device, 0, sizeof(ntfs_boot_sector_t), (uint8_t *)out) < 0) {
        return -1;
    }
    return ntfs_signature_ok(out) ? 0 : -1;
}


static int ntfs_probe(block_device_t *device) {
    ntfs_boot_sector_t boot;
    return ntfs_read_boot_sector(device, &boot) == 0;
}

static vfs_node_t *ntfs_mount(block_device_t *device) {
    ntfs_mount_t *mount = (ntfs_mount_t *)kmalloc(sizeof(ntfs_mount_t));
    if (!mount) return 0;
    memset(mount, 0, sizeof(ntfs_mount_t));

    if (ntfs_read_boot_sector(device, &mount->boot_sector) != 0) {
        kfree(mount);
        return 0;
    }

    mount->device = device;
    mount->cluster_size = mount->boot_sector.bytes_per_sector * mount->boot_sector.sectors_per_cluster;

    vfs_node_t *root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!root) {
        kfree(mount);
        return 0;
    }
    memset(root, 0, sizeof(vfs_node_t));
    strcpy(root->name, "ntfs");
    root->flags = FS_DIRECTORY;
    root->ptr = (vfs_node_t *)mount;
    return root;
}

static filesystem_driver_t ntfs_driver = {
    .name = "ntfs",
    .probe = ntfs_probe,
    .mount = ntfs_mount
};

void ntfs_register() {
    vfs_register_filesystem(&ntfs_driver);
}
