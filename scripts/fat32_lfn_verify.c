/* Standalone host-side verification tool for the VFAT LFN Unicode +
 * checksum logic added to kernel/fat32.c. Re-implements the exact same
 * algorithm (not the kernel build) against the raw bytes of a FAT32 image,
 * so it can be compiled and run on the host without booting QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t jump[3];
    char oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t dir_entries;
    uint16_t total_sectors_16;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
    uint8_t boot_code[420];
    uint16_t signature;
} fat32_boot_sector_t;

typedef struct {
    char name[11];
    uint8_t attr;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t size;
} fat32_dir_entry_t;

typedef struct {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_low;
    uint16_t name3[2];
} fat32_lfn_entry_t;
#pragma pack(pop)

#define FAT32_LFN_MAX_UNITS 260

typedef struct {
    uint16_t units[FAT32_LFN_MAX_UNITS];
    uint8_t checksum;
    int active;
    int valid;
} fat32_lfn_state_t;

static void fat32_lfn_reset(fat32_lfn_state_t *st) {
    memset(st->units, 0, sizeof(st->units));
    st->checksum = 0;
    st->active = 0;
    st->valid = 0;
}

static uint8_t fat32_lfn_checksum(const char short_name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) | ((sum & 0xFE) >> 1));
        sum = (uint8_t)(sum + (uint8_t)short_name[i]);
    }
    return sum;
}

static void fat32_lfn_feed(fat32_lfn_state_t *st, const fat32_lfn_entry_t *entry) {
    uint32_t order = entry->order & 0x1F;
    uint16_t chars[13];
    uint32_t pos;

    if (order == 0 || order > 20) {
        st->active = 0;
        return;
    }

    if (entry->order & 0x40) {
        fat32_lfn_reset(st);
        st->active = 1;
        st->valid = 1;
        st->checksum = entry->checksum;
    } else if (!st->active) {
        return;
    } else if (entry->checksum != st->checksum) {
        st->valid = 0;
    }

    pos = (order - 1) * 13;
    for (int i = 0; i < 5; i++) chars[i] = entry->name1[i];
    for (int i = 0; i < 6; i++) chars[5 + i] = entry->name2[i];
    for (int i = 0; i < 2; i++) chars[11 + i] = entry->name3[i];

    for (int i = 0; i < 13 && pos + (uint32_t)i < FAT32_LFN_MAX_UNITS - 1; i++) {
        uint16_t v = chars[i];
        if (v == 0x0000 || v == 0xFFFF) continue;
        st->units[pos + i] = v;
    }
}

static int fat32_lfn_validate(const fat32_lfn_state_t *st, const char short_name[11]) {
    if (!st->active || !st->valid) return 0;
    return fat32_lfn_checksum(short_name) == st->checksum;
}

static void fat32_lfn_to_utf8(const uint16_t *units, char *out, uint32_t out_size) {
    uint32_t opos = 0;
    for (uint32_t i = 0; i < FAT32_LFN_MAX_UNITS && units[i] != 0 && opos + 4 < out_size; i++) {
        uint16_t cp = units[i];
        if (cp < 0x80) {
            out[opos++] = (char)cp;
        } else if (cp < 0x800) {
            out[opos++] = (char)(0xC0 | (cp >> 6));
            out[opos++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[opos++] = (char)(0xE0 | (cp >> 12));
            out[opos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[opos++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[opos] = '\0';
}

static int is_lfn(const fat32_dir_entry_t *e) { return (e->attr & 0x0F) == 0x0F; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fat32-image>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }

    fat32_boot_sector_t bpb;
    if (fread(&bpb, sizeof(bpb), 1, f) != 1) { fprintf(stderr, "short read on BPB\n"); return 1; }

    uint32_t data_start_lba = bpb.reserved_sectors + (uint32_t)bpb.fat_count * bpb.sectors_per_fat_32;
    uint32_t cluster_size = (uint32_t)bpb.sectors_per_cluster * bpb.bytes_per_sector;
    uint32_t root_lba = data_start_lba + (bpb.root_cluster - 2) * bpb.sectors_per_cluster;
    long root_offset = (long)root_lba * bpb.bytes_per_sector;

    printf("bytes_per_sector=%u sectors_per_cluster=%u reserved_sectors=%u fat_count=%u sectors_per_fat_32=%u root_cluster=%u\n",
           bpb.bytes_per_sector, bpb.sectors_per_cluster, bpb.reserved_sectors, bpb.fat_count, bpb.sectors_per_fat_32, bpb.root_cluster);
    printf("data_start_lba=%u root_lba=%u root_offset=%ld cluster_size=%u\n", data_start_lba, root_lba, root_offset, cluster_size);

    uint8_t *buf = malloc(cluster_size);
    fseek(f, root_offset, SEEK_SET);
    if (fread(buf, cluster_size, 1, f) != 1) { fprintf(stderr, "short read on root dir\n"); return 1; }

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
    uint32_t n = cluster_size / sizeof(fat32_dir_entry_t);

    fat32_lfn_state_t lfn_state;
    fat32_lfn_reset(&lfn_state);

    printf("\n--- Directory entries ---\n");
    for (uint32_t i = 0; i < n; i++) {
        if ((unsigned char)entries[i].name[0] == 0x00) break;
        if (is_lfn(&entries[i])) {
            fat32_lfn_feed(&lfn_state, (fat32_lfn_entry_t *)&entries[i]);
            continue;
        }
        if ((unsigned char)entries[i].name[0] == 0xE5) {
            fat32_lfn_reset(&lfn_state);
            continue;
        }

        char short_name[12];
        memcpy(short_name, entries[i].name, 11);
        short_name[11] = '\0';

        int has_lfn = fat32_lfn_validate(&lfn_state, entries[i].name);
        char utf8[768];
        if (has_lfn) {
            fat32_lfn_to_utf8(lfn_state.units, utf8, sizeof(utf8));
        }

        printf("short=\"%s\" attr=0x%02x size=%u lfn_checksum_ok=%s display_name=\"%s\"\n",
               short_name, entries[i].attr, entries[i].size,
               has_lfn ? "YES" : "no",
               has_lfn ? utf8 : short_name);

        fat32_lfn_reset(&lfn_state);
    }

    free(buf);
    fclose(f);
    return 0;
}
