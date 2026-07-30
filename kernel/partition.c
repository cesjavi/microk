#include "partition.h"
#include "blockdev.h"
#include "kheap.h"
#include <string.h>

#define MBR_SIGNATURE_OFFSET 510
#define MBR_PARTITION_OFFSET 446
#define MBR_MAX_PRIMARY      4
#define GPT_MAX_SCAN_ENTRIES 16
#define SECTOR_SIZE          512

typedef struct {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t first_lba;
    uint32_t sectors;
} __attribute__((packed)) mbr_partition_entry_t;

typedef struct {
    char signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t partition_entry_count;
    uint32_t partition_entry_size;
    uint32_t partition_entry_array_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed)) gpt_partition_entry_t;

typedef struct {
    block_device_t device;
    block_device_t *parent;
    uint32_t first_lba;
    uint32_t sectors;
    uint8_t type;
    char name[16];
} partition_device_t;

static uint32_t partition_read(block_device_t *device, uint32_t offset, uint32_t size, uint8_t *buffer) {
    partition_device_t *part = (partition_device_t *)device->driver_data;
    if (!part || !part->parent || !buffer) return 0;

    /* first_lba/sectors come from an on-disk MBR/GPT entry; do the scaling
     * and final offset math in 64 bits so a partition that starts beyond
     * ~4GiB into the disk fails instead of silently wrapping to a smaller,
     * wrong 32-bit offset (the parent device's read() only takes a 32-bit
     * offset, so anything past that genuinely cannot be serviced here). */
    uint64_t start = (uint64_t)part->first_lba * SECTOR_SIZE;
    uint64_t limit = (uint64_t)part->sectors * SECTOR_SIZE;
    if (offset >= limit) return 0;
    if ((uint64_t)offset + size > limit) {
        size = (uint32_t)(limit - offset);
    }
    uint64_t final_offset = start + offset;
    if (final_offset > 0xFFFFFFFFULL) return 0;
    return part->parent->read(part->parent, (uint32_t)final_offset, size, buffer);
}

static void make_partition_name(char *out, const char *disk_name, int index) {
    int pos = 0;
    while (disk_name && *disk_name && pos < 10) {
        out[pos++] = *disk_name++;
    }
    out[pos++] = 'p';
    out[pos++] = (char)('0' + index);
    out[pos] = '\0';
}

int partition_scan_mbr(block_device_t *disk) {
    uint8_t mbr[SECTOR_SIZE];
    int found = 0;

    if (!disk || disk->read(disk, 0, SECTOR_SIZE, mbr) != SECTOR_SIZE) {
        return 0;
    }
    if (mbr[MBR_SIGNATURE_OFFSET] != 0x55 || mbr[MBR_SIGNATURE_OFFSET + 1] != 0xAA) {
        return 0;
    }

    mbr_partition_entry_t *entries = (mbr_partition_entry_t *)(mbr + MBR_PARTITION_OFFSET);
    for (int i = 0; i < MBR_MAX_PRIMARY; i++) {
        if (entries[i].type == 0xEE) {
            return partition_scan_gpt(disk);
        }
    }

    for (int i = 0; i < MBR_MAX_PRIMARY; i++) {
        if (entries[i].type == 0 || entries[i].sectors == 0) {
            continue;
        }

        partition_device_t *part = (partition_device_t *)kmalloc(sizeof(partition_device_t));
        if (!part) {
            continue;
        }
        memset(part, 0, sizeof(partition_device_t));
        part->parent = disk;
        part->first_lba = entries[i].first_lba;
        part->sectors = entries[i].sectors;
        part->type = entries[i].type;
        make_partition_name(part->name, disk->name, i + 1);

        part->device.name = part->name;
        part->device.block_size = disk->block_size;
        part->device.block_count = entries[i].sectors;
        part->device.driver_data = part;
        part->device.read = partition_read;

        if (blockdev_register(&part->device) == 0) {
            found++;
        }
    }

    return found;
}

static int guid_is_zero(uint8_t *guid) {
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) return 0;
    }
    return 1;
}

static int gpt_signature_ok(gpt_header_t *header) {
    return header->signature[0] == 'E' &&
           header->signature[1] == 'F' &&
           header->signature[2] == 'I' &&
           header->signature[3] == ' ' &&
           header->signature[4] == 'P' &&
           header->signature[5] == 'A' &&
           header->signature[6] == 'R' &&
           header->signature[7] == 'T';
}

int partition_scan_gpt(block_device_t *disk) {
    uint8_t sector[SECTOR_SIZE];
    gpt_header_t *header = (gpt_header_t *)sector;
    int found = 0;

    if (!disk || disk->read(disk, SECTOR_SIZE, SECTOR_SIZE, sector) != SECTOR_SIZE) {
        return 0;
    }
    if (!gpt_signature_ok(header) || header->partition_entry_size < sizeof(gpt_partition_entry_t)) {
        return 0;
    }

    uint32_t entries = header->partition_entry_count;
    if (entries > GPT_MAX_SCAN_ENTRIES) {
        entries = GPT_MAX_SCAN_ENTRIES;
    }

    for (uint32_t i = 0; i < entries; i++) {
        uint64_t entry_offset64 = header->partition_entry_lba * (uint64_t)SECTOR_SIZE +
                                   (uint64_t)i * header->partition_entry_size;
        uint8_t entry_buffer[128];
        if (header->partition_entry_size > sizeof(entry_buffer)) {
            continue;
        }
        if (entry_offset64 > 0xFFFFFFFFULL) {
            continue;
        }
        uint32_t entry_offset = (uint32_t)entry_offset64;
        if (disk->read(disk, entry_offset, header->partition_entry_size, entry_buffer) != header->partition_entry_size) {
            continue;
        }

        gpt_partition_entry_t *entry = (gpt_partition_entry_t *)entry_buffer;
        if (guid_is_zero(entry->type_guid) || entry->first_lba == 0 || entry->last_lba < entry->first_lba) {
            continue;
        }
        if (entry->first_lba > 0xFFFFFFFF || entry->last_lba > 0xFFFFFFFF) {
            continue;
        }

        partition_device_t *part = (partition_device_t *)kmalloc(sizeof(partition_device_t));
        if (!part) {
            continue;
        }
        memset(part, 0, sizeof(partition_device_t));
        part->parent = disk;
        part->first_lba = (uint32_t)entry->first_lba;
        part->sectors = (uint32_t)(entry->last_lba - entry->first_lba + 1);
        part->type = 0xEE;
        make_partition_name(part->name, disk->name, (int)i + 1);

        part->device.name = part->name;
        part->device.block_size = disk->block_size;
        part->device.block_count = part->sectors;
        part->device.driver_data = part;
        part->device.read = partition_read;

        if (blockdev_register(&part->device) == 0) {
            found++;
        }
    }

    return found;
}
