#include "boot_modules.h"
#include "extfs.h"
#include "fat32.h"
#include "initrd.h"
#include "llm.h"
#include "video.h"
#include <string.h>

#define MKLM_MAGIC 0x4D4C4B4D
#define GGUF_MAGIC 0x46554747

static int module_name_matches(const char *name, const char *token) {
    return name && token && strstr(name, token) != 0;
}

static int module_is_initrd(const char *name) {
    return module_name_matches(name, "initrd") ||
           module_name_matches(name, "ramdisk");
}

static int module_is_model(const char *name) {
    return module_name_matches(name, "model") ||
           module_name_matches(name, "llm") ||
           module_name_matches(name, "gguf") ||
           module_name_matches(name, "weights");
}

static int module_has_model_magic(multiboot_module_t *mod) {
    if (!mod || mod->mod_end <= mod->mod_start + sizeof(uint32_t)) {
        return 0;
    }

    uint32_t magic = *((uint32_t *)mod->mod_start);
    return magic == MKLM_MAGIC || magic == GGUF_MAGIC;
}

void boot_modules_load(multiboot_info_t *mbi) {
    int initrd_loaded = 0;
    int model_loaded = 0;

    if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
        multiboot_module_t *mods = (multiboot_module_t *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            const char *name = (const char *)mods[i].string;
            if (!initrd_loaded && module_is_initrd(name)) {
                klog("Loading Initrd Module...");
                initialise_initrd(mods[i].mod_start, mods[i].mod_end);
                initrd_loaded = 1;
            } else if (!model_loaded && (module_is_model(name) || module_has_model_magic(&mods[i]))) {
                klog("Loading LLM Model Module...");
                llm_load_model_module(mods[i].mod_start, mods[i].mod_end, name);
                model_loaded = 1;
            }
        }

        if (!initrd_loaded && mbi->mods_count == 1 && !model_loaded) {
            klog("Loading Initrd Module...");
            initialise_initrd(mods[0].mod_start, mods[0].mod_end);
        }
    }

    if (!model_loaded) {
        uint32_t start_addr;
        uint32_t size;

        klog("Trying to load LLM Model from FAT32 filesystem (/model.mklm)...");
        if (!fat32_is_mounted()) {
            klog("FAT32: no filesystem mounted; boot with -drive storage.img or run make qemu.");
        }
        if (fat32_find_model(&start_addr, &size)) {
            llm_load_model_module(start_addr, start_addr + size, "fat32_model");
            klog("Loaded LLM Model from FAT32.");
        } else if (extfs_load_file("model.mklm", (uint8_t *)0x2000000, 16 * 1024 * 1024, &size) ||
                   extfs_load_file("models/model.mklm", (uint8_t *)0x2000000, 16 * 1024 * 1024, &size)) {
            start_addr = 0x2000000;
            llm_load_model_module(start_addr, start_addr + size, "ext_model");
            klog("Loaded LLM Model from EXT.");
        } else {
            klog("Failed to load model from FAT32/EXT. Tried /model.mklm and /models/model.mklm.");
        }
    }
}
