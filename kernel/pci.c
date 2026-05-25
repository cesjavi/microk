#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int pci_devices_found;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t address = (uint32_t)((1U << 31) |
                                  ((uint32_t)bus << 16) |
                                  ((uint32_t)slot << 11) |
                                  ((uint32_t)function << 8) |
                                  (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t value = pci_config_read32(bus, slot, function, offset);
    return (uint16_t)((value >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((1U << 31) |
                                  ((uint32_t)bus << 16) |
                                  ((uint32_t)slot << 11) |
                                  ((uint32_t)function << 8) |
                                  (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static void pci_add_device(uint8_t bus, uint8_t slot, uint8_t function) {
    if (pci_devices_found >= PCI_MAX_DEVICES) return;

    uint32_t id = pci_config_read32(bus, slot, function, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    if (vendor == 0xFFFF) return;

    pci_device_t *device = &pci_devices[pci_devices_found++];
    device->bus = bus;
    device->slot = slot;
    device->function = function;
    device->vendor_id = vendor;
    device->device_id = (uint16_t)(id >> 16);

    uint32_t class_info = pci_config_read32(bus, slot, function, 0x08);
    device->prog_if = (uint8_t)((class_info >> 8) & 0xFF);
    device->subclass = (uint8_t)((class_info >> 16) & 0xFF);
    device->class_code = (uint8_t)((class_info >> 24) & 0xFF);

    uint32_t header = pci_config_read32(bus, slot, function, 0x0C);
    device->header_type = (uint8_t)((header >> 16) & 0xFF);

    for (int i = 0; i < 6; i++) {
        device->bar[i] = pci_config_read32(bus, slot, function, (uint8_t)(0x10 + i * 4));
    }
}

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t function) {
    pci_add_device(bus, slot, function);
}

static void pci_scan_slot(uint8_t bus, uint8_t slot) {
    uint16_t vendor = pci_config_read16(bus, slot, 0, 0x00);
    if (vendor == 0xFFFF) return;

    pci_scan_function(bus, slot, 0);
    uint8_t header_type = (uint8_t)((pci_config_read32(bus, slot, 0, 0x0C) >> 16) & 0xFF);
    if (header_type & 0x80) {
        for (uint8_t function = 1; function < 8; function++) {
            if (pci_config_read16(bus, slot, function, 0x00) != 0xFFFF) {
                pci_scan_function(bus, slot, function);
            }
        }
    }
}

void pci_init() {
    pci_devices_found = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            pci_scan_slot((uint8_t)bus, slot);
        }
    }
}

int pci_device_count() {
    return pci_devices_found;
}

pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= pci_devices_found) {
        return 0;
    }
    return &pci_devices[index];
}
