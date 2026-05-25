#include "blockdev.h"

static block_device_t *devices[MAX_BLOCK_DEVICES];
static int device_count;

void blockdev_init() {
    device_count = 0;
    for (int i = 0; i < MAX_BLOCK_DEVICES; i++) {
        devices[i] = 0;
    }
}

int blockdev_register(block_device_t *device) {
    if (!device || !device->read || device->block_size == 0) {
        return -1;
    }
    if (device_count >= MAX_BLOCK_DEVICES) {
        return -1;
    }
    devices[device_count++] = device;
    return 0;
}

int blockdev_count() {
    return device_count;
}

block_device_t *blockdev_get(int index) {
    if (index < 0 || index >= device_count) {
        return 0;
    }
    return devices[index];
}
