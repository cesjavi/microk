#ifndef EXTFS_H
#define EXTFS_H

#include "vfs.h"

void extfs_register();
void extfs_ls();
void extfs_cat(const char *filename);
int extfs_load_file(const char *filename, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size);

#endif
