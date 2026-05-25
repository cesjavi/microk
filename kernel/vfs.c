#include "vfs.h"

vfs_node_t *fs_root = 0;
static filesystem_driver_t *filesystem_drivers[VFS_MAX_FILESYSTEMS];
static int filesystem_driver_count = 0;

void vfs_init() {
    filesystem_driver_count = 0;
    for (int i = 0; i < VFS_MAX_FILESYSTEMS; i++) {
        filesystem_drivers[i] = 0;
    }
}

int vfs_register_filesystem(filesystem_driver_t *driver) {
    if (!driver || !driver->name || !driver->probe || !driver->mount) {
        return -1;
    }
    if (filesystem_driver_count >= VFS_MAX_FILESYSTEMS) {
        return -1;
    }
    filesystem_drivers[filesystem_driver_count++] = driver;
    return 0;
}

vfs_node_t *vfs_mount(block_device_t *device, const char **fs_name) {
    if (fs_name) *fs_name = 0;
    if (!device || !device->read) return 0;

    for (int i = 0; i < filesystem_driver_count; i++) {
        filesystem_driver_t *driver = filesystem_drivers[i];
        if (driver->probe(device)) {
            if (fs_name) *fs_name = driver->name;
            return driver->mount(device);
        }
    }
    return 0;
}

int vfs_read_device(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!device || !device->read || !buffer) {
        return -1;
    }
    if (device->block_size == 0) {
        return -1;
    }
    return (int)device->read(device, offset, size, buffer);
}

int vfs_write_device(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!device || !device->write || !buffer) {
        return -1;
    }
    if (device->block_size == 0) {
        return -1;
    }
    return (int)device->write(device, offset, size, buffer);
}

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (node && node->read) {
        return node->read(node, offset, size, buffer);
    }
    return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (node && node->write) {
        return node->write(node, offset, size, buffer);
    }
    return 0;
}
