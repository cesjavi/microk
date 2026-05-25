#include "initrd.h"
#include "kheap.h"
#include <string.h>

vfs_node_t *initrd_root;
vfs_node_t *initrd_dev;
vfs_node_t *root_nodes;
int nroot_nodes;

initrd_header_t *initrd_header;
initrd_file_header_t *file_headers;

static uint32_t initrd_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    uint32_t index = (uint32_t)(uintptr_t)node->ptr;
    initrd_file_header_t header = file_headers[index];
    if (offset > header.length) return 0;
    if (offset + size > header.length) size = header.length - offset;
    memcpy(buffer, (uint8_t *)(header.offset + offset), size);
    return size;
}

vfs_node_t *initialise_initrd(uint32_t location) {
    initrd_header = (initrd_header_t *)location;
    file_headers = (initrd_file_header_t *)(location + sizeof(initrd_header_t));

    initrd_root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!initrd_root) return 0;
    strcpy(initrd_root->name, "initrd");
    initrd_root->flags = FS_DIRECTORY;
    initrd_root->length = 0;
    initrd_root->read = 0;
    initrd_root->write = 0;
    initrd_root->ptr = 0;

    nroot_nodes = initrd_header->nfiles;
    root_nodes = (vfs_node_t *)kmalloc(sizeof(vfs_node_t) * nroot_nodes);
    if (!root_nodes) return initrd_root;

    for (int i = 0; i < nroot_nodes; i++) {
        file_headers[i].offset += location;
        strcpy(root_nodes[i].name, file_headers[i].name);
        root_nodes[i].length = file_headers[i].length;
        root_nodes[i].flags = FS_FILE;
        root_nodes[i].read = &initrd_read;
        root_nodes[i].write = 0;
        root_nodes[i].ptr = (vfs_node_t *)(uintptr_t)i;
    }
    
    fs_root = initrd_root;
    return initrd_root;
}

vfs_node_t *initrd_find_node(const char *name) {
    if (!name || !root_nodes) return NULL;
    for (int i = 0; i < nroot_nodes; i++) {
        if (strcmp(name, root_nodes[i].name) == 0) {
            return &root_nodes[i];
        }
    }
    return NULL;
}
