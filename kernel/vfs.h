#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02

#define VFS_MAX_FILESYSTEMS 8

struct vfs_node;
struct block_device;

typedef uint32_t (*read_type_t)(struct vfs_node*, uint32_t, uint32_t, uint8_t*);
typedef uint32_t (*write_type_t)(struct vfs_node*, uint32_t, uint32_t, uint8_t*);
typedef uint32_t (*block_read_t)(struct block_device*, uint32_t, uint32_t, uint8_t*);
typedef uint32_t (*block_write_t)(struct block_device*, uint32_t, uint32_t, uint8_t*);

typedef struct vfs_node {
    char name[128];
    uint32_t flags;
    uint32_t length;
    read_type_t read;
    write_type_t write;
    struct vfs_node *ptr; // For directories or links
} vfs_node_t;

typedef struct block_device {
    const char *name;
    uint32_t block_size;
    uint32_t block_count;
    void *driver_data;
    block_read_t read;
    block_write_t write;
} block_device_t;

typedef struct filesystem_driver {
    const char *name;
    int (*probe)(block_device_t *device);
    vfs_node_t *(*mount)(block_device_t *device);
} filesystem_driver_t;

extern vfs_node_t *fs_root;

void vfs_init();
int vfs_register_filesystem(filesystem_driver_t *driver);
vfs_node_t *vfs_mount(block_device_t *device, const char **fs_name);
int vfs_read_device(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer);
int vfs_write_device(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);

#endif
