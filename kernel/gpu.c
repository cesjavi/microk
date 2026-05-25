#include "gpu.h"
#include "pci.h"
#include "video.h"
#include "vmm.h"

#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_BRIDGE 0x06
#define PCI_SUBCLASS_VGA 0x00
#define PCI_SUBCLASS_3D 0x02
#define PCI_VENDOR_NVIDIA 0x10DE
#define PCI_VENDOR_AMD_ATI 0x1002
#define PCI_VENDOR_AMD 0x1022

#define NVIDIA_BAR0_VMEM 0xFD000000
#define NV_PMC_BOOT_0    0x00000000

static gpu_info_t active_gpu;
static char gpu_status_buf[768];

static uint32_t pci_bar_address(uint32_t bar) {
    if (bar & 0x1) {
        return bar & ~0x3;
    }
    return bar & ~0xF;
}

static gpu_vendor_t classify_vendor(uint16_t vendor_id, const char **name) {
    if (vendor_id == PCI_VENDOR_NVIDIA) {
        *name = "NVIDIA GPU";
        return GPU_VENDOR_NVIDIA;
    }
    if (vendor_id == PCI_VENDOR_AMD_ATI || vendor_id == PCI_VENDOR_AMD) {
        *name = "AMD GPU";
        return GPU_VENDOR_AMD;
    }
    *name = "Generic VGA/Display GPU";
    return GPU_VENDOR_GENERIC;
}

static int is_display_device(pci_device_t *device) {
    if (device->class_code == PCI_CLASS_DISPLAY) {
        return 1;
    }
    if (device->class_code == PCI_CLASS_BRIDGE && device->subclass == PCI_SUBCLASS_VGA) {
        return 1;
    }
    return 0;
}

static const char *gpu_support_string(gpu_vendor_t vendor) {
    if (vendor == GPU_VENDOR_NVIDIA) {
        return "NVIDIA PCI diagnostics only; native framebuffer/compute not implemented";
    }
    if (vendor == GPU_VENDOR_AMD) {
        return "AMD PCI diagnostics only; native framebuffer/compute not implemented";
    }
    return "generic VGA text output only";
}

static uint32_t pci_bar_size_mask(uint32_t raw_bar) {
    if (raw_bar & 0x1) {
        return raw_bar & ~0x3;
    }
    return raw_bar & ~0xF;
}

static uint32_t pci_bar_size_from_probe(uint32_t value) {
    uint32_t mask = pci_bar_size_mask(value);
    if (mask == 0 || mask == 0xFFFFFFFF) {
        return 0;
    }
    return (~mask) + 1;
}

static gpu_bar_info_t read_bar_info(pci_device_t *device, int index) {
    gpu_bar_info_t info;
    uint8_t offset = (uint8_t)(0x10 + index * 4);
    uint32_t original = device->bar[index];
    uint16_t command = pci_config_read16(device->bus, device->slot, device->function, 0x04);
    uint32_t command_status = pci_config_read32(device->bus, device->slot, device->function, 0x04);
    uint32_t size_probe;

    info.raw = original;
    info.present = original != 0 && original != 0xFFFFFFFF;
    info.io = (original & 0x1) ? 1 : 0;
    info.mem64 = (!info.io && ((original >> 1) & 0x3) == 0x2) ? 1 : 0;
    info.prefetchable = (!info.io && (original & 0x8)) ? 1 : 0;
    info.address = info.present ? pci_bar_address(original) : 0;
    info.size = 0;

    if (!info.present) {
        return info;
    }

    pci_config_write32(device->bus, device->slot, device->function, 0x04, command_status & ~0x3);
    pci_config_write32(device->bus, device->slot, device->function, offset, 0xFFFFFFFF);
    size_probe = pci_config_read32(device->bus, device->slot, device->function, offset);
    pci_config_write32(device->bus, device->slot, device->function, offset, original);
    pci_config_write32(device->bus, device->slot, device->function, 0x04, (command_status & 0xFFFF0000) | command);

    info.size = pci_bar_size_from_probe(size_probe);
    return info;
}

static void append_char(char **cursor, char *end, char c) {
    if (*cursor < end - 1) {
        **cursor = c;
        (*cursor)++;
        **cursor = '\0';
    }
}

static void append_str(char **cursor, char *end, const char *text) {
    while (text && *text) {
        append_char(cursor, end, *text++);
    }
}

static void append_hex_digit(char **cursor, char *end, uint8_t value) {
    append_char(cursor, end, value < 10 ? ('0' + value) : ('A' + value - 10));
}

static void append_hex32(char **cursor, char *end, uint32_t value, int digits) {
    append_str(cursor, end, "0x");
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        append_hex_digit(cursor, end, (value >> shift) & 0xF);
    }
}

static void append_uint(char **cursor, char *end, uint32_t value) {
    char tmp[11];
    int pos = 10;

    tmp[pos] = '\0';
    if (value == 0) {
        append_char(cursor, end, '0');
        return;
    }

    while (value > 0 && pos > 0) {
        tmp[--pos] = '0' + (value % 10);
        value /= 10;
    }

    append_str(cursor, end, &tmp[pos]);
}

static void append_bar(char **cursor, char *end, int index, const gpu_bar_info_t *bar) {
    append_str(cursor, end, "  bar");
    append_uint(cursor, end, (uint32_t)index);
    append_str(cursor, end, ": ");
    if (!bar->present) {
        append_str(cursor, end, "none\n");
        return;
    }
    append_str(cursor, end, bar->io ? "io" : (bar->mem64 ? "mem64" : "mem32"));
    if (bar->prefetchable) {
        append_str(cursor, end, " prefetch");
    }
    if (!bar->io) {
        if (bar->prefetchable) {
            append_str(cursor, end, " [VRAM Framebuffer]");
        } else {
            append_str(cursor, end, " [MMIO]");
        }
    }
    append_str(cursor, end, " addr=");
    append_hex32(cursor, end, bar->address, 8);
    append_str(cursor, end, " size=");
    if (bar->size) {
        append_uint(cursor, end, bar->size / 1024);
        append_str(cursor, end, " KiB");
    } else {
        append_str(cursor, end, "unknown");
    }
    append_str(cursor, end, " raw=");
    append_hex32(cursor, end, bar->raw, 8);
    append_char(cursor, end, '\n');
}

static const char *decode_nvidia_family(uint32_t pmc_boot) {
    uint32_t top = (pmc_boot >> 24) & 0xFF;
    
    if (top == 0x0E || top == 0x0F || top == 0x10) {
        return "Kepler (GeForce 600/700)";
    }
    if (top == 0x11 || top == 0x12) {
        return "Maxwell (GeForce 750/900)";
    }
    if (top == 0x13) {
        return "Pascal (GeForce 10)";
    }
    if (top == 0x14) {
        return "Volta (Titan V)";
    }
    if (top == 0x16) {
        return "Turing (GeForce 16/20)";
    }
    if (top == 0x17) {
        return "Ampere (GeForce 30)";
    }
    if (top == 0x18 || top == 0x19) {
        return "Ada Lovelace (GeForce 40)";
    }
    return "Unknown/Unsupported NVIDIA Architecture";
}

void gpu_init() {
    active_gpu.present = 0;
    active_gpu.vendor = GPU_VENDOR_NONE;
    active_gpu.name = "No GPU detected";
    active_gpu.support = "VGA text mode";
    active_gpu.pmc_boot_0 = 0;
    active_gpu.nvidia_family = 0;
    active_gpu.mapped_vmem = 0;
    active_gpu.bar0_mapped = 0;

    for (int i = 0; i < pci_device_count(); i++) {
        pci_device_t *device = pci_get_device(i);
        if (!device || !is_display_device(device)) {
            continue;
        }

        const char *name = 0;
        gpu_vendor_t vendor = classify_vendor(device->vendor_id, &name);

        active_gpu.present = 1;
        active_gpu.vendor = vendor;
        active_gpu.vendor_id = device->vendor_id;
        active_gpu.device_id = device->device_id;
        active_gpu.bus = device->bus;
        active_gpu.slot = device->slot;
        active_gpu.function = device->function;
        active_gpu.class_code = device->class_code;
        active_gpu.subclass = device->subclass;
        active_gpu.prog_if = device->prog_if;
        active_gpu.command = pci_config_read16(device->bus, device->slot, device->function, 0x04);
        active_gpu.status = pci_config_read16(device->bus, device->slot, device->function, 0x06);
        active_gpu.revision_id = (uint8_t)(pci_config_read32(device->bus, device->slot, device->function, 0x08) & 0xFF);
        uint32_t irq = pci_config_read32(device->bus, device->slot, device->function, 0x3C);
        active_gpu.interrupt_line = (uint8_t)(irq & 0xFF);
        active_gpu.interrupt_pin = (uint8_t)((irq >> 8) & 0xFF);
        for (int bar = 0; bar < 6; bar++) {
            active_gpu.bar[bar] = read_bar_info(device, bar);
        }
        active_gpu.name = name;
        active_gpu.support = gpu_support_string(vendor);

        if (vendor == GPU_VENDOR_NVIDIA) {
            klog("GPU: NVIDIA display device detected (PCI diagnostics mode).");
            if (active_gpu.bar[0].present && !active_gpu.bar[0].io) {
                uint32_t phys_addr = active_gpu.bar[0].address;
                uint32_t size = active_gpu.bar[0].size;
                if (size == 0) {
                    size = 16 * 1024 * 1024; // 16 MB fallback
                }
                
                active_gpu.mapped_vmem = NVIDIA_BAR0_VMEM;
                active_gpu.bar0_mapped = 0;
                
                // Map page-by-page (limit to max 16MB / 4096 pages for stability)
                uint32_t num_pages = size / 4096;
                if (num_pages > 4096) {
                    num_pages = 4096;
                }
                
                for (uint32_t p = 0; p < num_pages; p++) {
                    vmm_map_page_ext(NVIDIA_BAR0_VMEM + p * 4096, phys_addr + p * 4096, 0x1B);
                }
                active_gpu.bar0_mapped = 1;
                klog("GPU: NVIDIA BAR0 MMIO successfully mapped.");
                
                // Safe read of NV_PMC_BOOT_0
                volatile uint32_t *pmc_boot_ptr = (volatile uint32_t *)(NVIDIA_BAR0_VMEM + NV_PMC_BOOT_0);
                active_gpu.pmc_boot_0 = *pmc_boot_ptr;
                
                // Decode architecture family
                active_gpu.nvidia_family = decode_nvidia_family(active_gpu.pmc_boot_0);
            }
        } else if (vendor == GPU_VENDOR_AMD) {
            klog("GPU: AMD display device detected.");
        } else {
            klog("GPU: generic display device detected.");
        }
        return;
    }

    klog("GPU: no PCI display device detected; using VGA text mode.");
}

const gpu_info_t *gpu_get_info() {
    return &active_gpu;
}

const char *gpu_status_string() {
    char *cursor = gpu_status_buf;
    char *end = gpu_status_buf + sizeof(gpu_status_buf);

    gpu_status_buf[0] = '\0';
    if (!active_gpu.present) {
        append_str(&cursor, end, "GPU:\n");
        append_str(&cursor, end, "  present: no\n");
        append_str(&cursor, end, "  output: VGA text mode\n");
        return gpu_status_buf;
    }

    append_str(&cursor, end, "GPU:\n");
    append_str(&cursor, end, "  present: yes\n");
    append_str(&cursor, end, "  name: ");
    append_str(&cursor, end, active_gpu.name);
    append_char(&cursor, end, '\n');
    append_str(&cursor, end, "  vendor: ");
    append_hex32(&cursor, end, active_gpu.vendor_id, 4);
    append_str(&cursor, end, "\n  device: ");
    append_hex32(&cursor, end, active_gpu.device_id, 4);
    append_str(&cursor, end, "\n  pci: ");
    append_uint(&cursor, end, active_gpu.bus);
    append_char(&cursor, end, ':');
    append_uint(&cursor, end, active_gpu.slot);
    append_char(&cursor, end, '.');
    append_uint(&cursor, end, active_gpu.function);
    append_str(&cursor, end, "\n  class: ");
    append_hex32(&cursor, end, active_gpu.class_code, 2);
    append_char(&cursor, end, '/');
    append_hex32(&cursor, end, active_gpu.subclass, 2);
    append_str(&cursor, end, " prog_if=");
    append_hex32(&cursor, end, active_gpu.prog_if, 2);
    append_str(&cursor, end, " rev=");
    append_hex32(&cursor, end, active_gpu.revision_id, 2);
    append_str(&cursor, end, "\n  command/status: ");
    append_hex32(&cursor, end, active_gpu.command, 4);
    append_char(&cursor, end, '/');
    append_hex32(&cursor, end, active_gpu.status, 4);
    append_str(&cursor, end, "\n  irq: line=");
    append_uint(&cursor, end, active_gpu.interrupt_line);
    append_str(&cursor, end, " pin=");
    append_uint(&cursor, end, active_gpu.interrupt_pin);
    append_char(&cursor, end, '\n');
    append_bar(&cursor, end, 0, &active_gpu.bar[0]);
    append_bar(&cursor, end, 1, &active_gpu.bar[1]);
    append_bar(&cursor, end, 2, &active_gpu.bar[2]);

    if (active_gpu.vendor == GPU_VENDOR_NVIDIA) {
        append_str(&cursor, end, "\n  NVIDIA Diagnostics:\n");
        append_str(&cursor, end, "    BAR0 MMIO Virtual Mapping: ");
        if (active_gpu.bar0_mapped) {
            append_hex32(&cursor, end, active_gpu.mapped_vmem, 8);
            append_str(&cursor, end, "\n    Register NV_PMC_BOOT_0:    ");
            append_hex32(&cursor, end, active_gpu.pmc_boot_0, 8);
            append_str(&cursor, end, "\n    Decoded Architecture:      ");
            append_str(&cursor, end, active_gpu.nvidia_family ? active_gpu.nvidia_family : "unknown");
            append_char(&cursor, end, '\n');
        } else {
            append_str(&cursor, end, "Not Mapped\n");
        }
    }

    append_str(&cursor, end, "\n  output: VGA text mode\n");
    append_str(&cursor, end, "  support: ");
    append_str(&cursor, end, active_gpu.support);
    append_char(&cursor, end, '\n');
    return gpu_status_buf;
}
