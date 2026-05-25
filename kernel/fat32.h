#ifndef FAT32_H
#define FAT32_H

#include "vfs.h"
#include "kheap.h"
#include "string.h"

void fat32_register();
int fat32_is_mounted();
int fat32_find_model(uint32_t *start_addr, uint32_t *size);
int fat32_load_file(const char *filename, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size);
uint32_t fat32_get_size(const char *filename);
int fat32_get_entry_by_index(const char *path, uint32_t index, char *out_name, int *out_is_dir);
int fat32_path_is_dir(const char *path);
void fat32_ls();
void fat32_ls_path(const char *path);
void fat32_cat(const char *filename);
int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t size);
int fat32_create_file(const char *filename);
int fat32_delete_file(const char *filename);
int fat32_mkdir(const char *path);

#endif
