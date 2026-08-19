#ifndef GPU_H
#define GPU_H

#include <stdint.h>

typedef enum {
    GPU_VENDOR_NONE = 0,
    GPU_VENDOR_NVIDIA,
    GPU_VENDOR_AMD,
    GPU_VENDOR_GENERIC
} gpu_vendor_t;

typedef struct {
    uint32_t raw;
    int present;
    int io;
    int mem64;
    int prefetchable;
    uint32_t address;
    uint32_t size;
} gpu_bar_info_t;

typedef struct {
    int present;
    gpu_vendor_t vendor;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    gpu_bar_info_t bar[6];
    const char *name;
    const char *support;
    
    /* NVIDIA BAR0 diagnostics & mapping */
    uint32_t pmc_boot_0;
    const char *nvidia_family;
    uint32_t mapped_vmem;
    int bar0_mapped;

    /* VRAM size, decoded only for chip families verified against real
     * Nouveau source (see decode_pascal_vidmem_size in gpu.c) -- 0/unknown
     * for anything else rather than guessing at a formula we haven't
     * confirmed for that generation. */
    uint64_t vram_size_bytes;
    int vram_size_known;
} gpu_info_t;

void gpu_init();
const gpu_info_t *gpu_get_info();
const char *gpu_status_string();

#endif
