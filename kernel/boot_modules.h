#ifndef BOOT_MODULES_H
#define BOOT_MODULES_H

#include "multiboot.h"

void boot_modules_load(multiboot_info_t *mbi);
int boot_module_find(multiboot_info_t *mbi, const char *name_token,
                     uint32_t *start, uint32_t *size);

#endif
