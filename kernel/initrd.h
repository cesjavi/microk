#ifndef INITRD_H
#define INITRD_H

#include "vfs.h"

// Basic initrd header
typedef struct {
    uint32_t nfiles;
} initrd_header_t;

typedef struct {
    uint8_t magic;
    char name[64];
    uint32_t offset;
    uint32_t length;
} initrd_file_header_t;

vfs_node_t *initialise_initrd(uint32_t location);
vfs_node_t *initrd_find_node(const char *name);

#endif
