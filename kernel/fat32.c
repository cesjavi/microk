#include "fat32.h"
#include "video.h"

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
} __attribute__((packed)) fat32_dir_entry_t;

typedef struct {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_low;
    uint16_t name3[2];
} __attribute__((packed)) fat32_lfn_entry_t;

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
    // FAT32 extended
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
} __attribute__((packed)) fat32_boot_sector_t;

typedef struct {
    block_device_t *device;
    fat32_boot_sector_t bpb;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t root_cluster;
} fat32_mount_t;

typedef struct {
    vfs_node_t node;
    fat32_mount_t *mount;
    uint32_t first_cluster;
} fat32_node_t;

/**
 * @brief Checks if a block device contains a valid FAT32 filesystem.
 */
static int fat32_probe(block_device_t *device) {
    fat32_boot_sector_t bpb;
    if (vfs_read_device(device, 0, sizeof(fat32_boot_sector_t), (uint8_t *)&bpb) < 0) return 0;
    if (bpb.signature != 0xAA55) return 0;
    if (bpb.sectors_per_fat_16 != 0) return 0; // Not FAT32
    if (bpb.boot_signature != 0x28 && bpb.boot_signature != 0x29) return 0;
    return 1;
}

/**
 * @brief Converts a FAT32 cluster number to a Logical Block Address (LBA).
 */
static uint32_t fat32_cluster_to_lba(fat32_mount_t *mount, uint32_t cluster) {
    return mount->data_start_lba + (cluster - 2) * mount->bpb.sectors_per_cluster;
}

/**
 * @brief Retrieves the next cluster in a chain from the File Allocation Table.
 */
static uint32_t fat32_get_next_cluster(fat32_mount_t *mount, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = mount->fat_start_lba + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;
    uint8_t sector[512];
    vfs_read_device(mount->device, fat_sector * 512, 512, sector);
    uint32_t next = *(uint32_t *)&sector[ent_offset] & 0x0FFFFFFF;
    return next;
}

/**
 * @brief Sets a value in the File Allocation Table for a specific cluster.
 * Used for linking clusters in a chain or marking them as free/EOC.
 */
static void fat32_set_cluster(fat32_mount_t *mount, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = mount->fat_start_lba + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;
    uint8_t sector[512];
    vfs_read_device(mount->device, fat_sector * 512, 512, sector);
    
    uint32_t current = *(uint32_t *)&sector[ent_offset];
    value = (value & 0x0FFFFFFF) | (current & 0xF0000000);
    *(uint32_t *)&sector[ent_offset] = value;
    
    vfs_write_device(mount->device, fat_sector * 512, 512, sector);
}

/**
 * @brief Scans the FAT to find the first available free cluster.
 * @return The cluster index or 0xFFFFFFFF if the disk is full.
 */
static uint32_t fat32_find_free_cluster(fat32_mount_t *mount) {
    uint8_t sector[512];
    uint32_t total_sectors_fat = mount->bpb.sectors_per_fat_32;
    uint32_t entries_per_sector = 512 / 4;

    for (uint32_t s = 0; s < total_sectors_fat; s++) {
        vfs_read_device(mount->device, (mount->fat_start_lba + s) * 512, 512, sector);
        uint32_t *entries = (uint32_t *)sector;
        for (uint32_t e = 0; e < entries_per_sector; e++) {
            uint32_t cluster = (s * entries_per_sector) + e;
            if (cluster < 2) continue; // Reserved
            if ((entries[e] & 0x0FFFFFFF) == 0) {
                return cluster;
            }
        }
    }
    return 0xFFFFFFFF;
}

/**
 * @brief Reads data from a FAT32 file node into a buffer.
 */
static uint32_t fat32_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    fat32_node_t *fnode = (fat32_node_t *)node;
    fat32_mount_t *mount = fnode->mount;
    uint32_t cluster = fnode->first_cluster;
    uint32_t cluster_size = mount->bpb.sectors_per_cluster * 512;
    
    // Skip clusters
    uint32_t cluster_idx = offset / cluster_size;
    uint32_t offset_in_cluster = offset % cluster_size;
    
    for (uint32_t i = 0; i < cluster_idx; i++) {
        cluster = fat32_get_next_cluster(mount, cluster);
        if (cluster >= 0x0FFFFFF8) return 0; // EOF
    }
    
    uint32_t bytes_read = 0;
    uint8_t *temp = kmalloc(cluster_size);
    if (!temp) return 0;

    while (bytes_read < size && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(mount, cluster);
        vfs_read_device(mount->device, lba * 512, cluster_size, temp);
        
        uint32_t chunk = cluster_size - offset_in_cluster;
        if (chunk > size - bytes_read) chunk = size - bytes_read;
        
        memcpy(buffer + bytes_read, temp + offset_in_cluster, chunk);
        bytes_read += chunk;
        offset_in_cluster = 0;
        cluster = fat32_get_next_cluster(mount, cluster);
    }
    kfree(temp);
    return bytes_read;
}

/**
 * @brief Writes data to a FAT32 file node.
 * Supports growing the file by allocating new clusters if writing past EOF.
 */
static uint32_t fat32_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    fat32_node_t *fnode = (fat32_node_t *)node;
    fat32_mount_t *mount = fnode->mount;
    uint32_t cluster = fnode->first_cluster;
    uint32_t cluster_size = mount->bpb.sectors_per_cluster * 512;
    
    // Skip clusters
    uint32_t cluster_idx = offset / cluster_size;
    uint32_t offset_in_cluster = offset % cluster_size;
    
    for (uint32_t i = 0; i < cluster_idx; i++) {
        cluster = fat32_get_next_cluster(mount, cluster);
        if (cluster >= 0x0FFFFFF8) return 0; // EOF (resizing not supported yet)
    }
    
    uint32_t bytes_written = 0;
    uint8_t *temp = kmalloc(cluster_size);
    if (!temp) return 0;

    while (bytes_written < size && cluster < 0x0FFFFFF8) {
        uint32_t lba = fat32_cluster_to_lba(mount, cluster);
        
        // Read-modify-write for the cluster
        vfs_read_device(mount->device, lba * 512, cluster_size, temp);
        
        uint32_t chunk = cluster_size - offset_in_cluster;
        if (chunk > size - bytes_written) chunk = size - bytes_written;
        
        memcpy(temp + offset_in_cluster, buffer + bytes_written, chunk);
        vfs_write_device(mount->device, lba * 512, cluster_size, temp);
        
        bytes_written += chunk;
        offset_in_cluster = 0;
        
        uint32_t next = fat32_get_next_cluster(mount, cluster);
        if (next >= 0x0FFFFFF8 && bytes_written < size) {
            // Allocate new cluster
            uint32_t free = fat32_find_free_cluster(mount);
            if (free == 0xFFFFFFFF) break; // Disk full
            
            fat32_set_cluster(mount, cluster, free);
            fat32_set_cluster(mount, free, 0x0FFFFFFF); // EOC
            next = free;
        }
        cluster = next;
    }
    kfree(temp);
    return bytes_written;
}

static fat32_mount_t *global_fat32_mount = 0;

static int fat32_is_eoc(uint32_t cluster) {
    return cluster >= 0x0FFFFFF8;
}

static int fat32_valid_cluster(uint32_t cluster) {
    return cluster >= 2 && !fat32_is_eoc(cluster);
}

static char fat32_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return c - ('a' - 'A');
    }
    return c;
}

static int fat32_char_equal_ci(char a, char b) {
    return fat32_upper(a) == fat32_upper(b);
}

static int fat32_str_equal_ci(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }

    while (*a && *b) {
        if (!fat32_char_equal_ci(*a, *b)) {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static int fat32_make_83_name(const char *filename, char out[11]) {
    int name_pos = 0;
    int ext_pos = 8;
    int in_ext = 0;

    if (!filename || !filename[0]) {
        return 0;
    }

    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    for (int i = 0; filename[i]; i++) {
        char c = filename[i];

        if (c == '/' || c == '\\') {
            name_pos = 0;
            ext_pos = 8;
            in_ext = 0;
            for (int j = 0; j < 11; j++) {
                out[j] = ' ';
            }
            continue;
        }

        if (c == '.') {
            in_ext = 1;
            continue;
        }

        if (c == ' ') {
            continue;
        }

        if (!in_ext) {
            if (name_pos >= 8) {
                return 0;
            }
            out[name_pos++] = fat32_upper(c);
        } else {
            if (ext_pos >= 11) {
                return 0;
            }
            out[ext_pos++] = fat32_upper(c);
        }
    }

    return name_pos > 0;
}

static void fat32_format_entry_name(const fat32_dir_entry_t *entry, char *out) {
    int pos = 0;
    int end = 7;

    while (end >= 0 && entry->name[end] == ' ') {
        end--;
    }
    for (int i = 0; i <= end; i++) {
        out[pos++] = entry->name[i];
    }

    if (entry->name[8] != ' ') {
        out[pos++] = '.';
        end = 10;
        while (end >= 8 && entry->name[end] == ' ') {
            end--;
        }
        for (int i = 8; i <= end; i++) {
            out[pos++] = entry->name[i];
        }
    }

    out[pos] = '\0';
}

static int fat32_entry_is_usable(const fat32_dir_entry_t *entry) {
    if ((unsigned char)entry->name[0] == 0x00) {
        return 0;
    }
    if ((unsigned char)entry->name[0] == 0xE5) {
        return 0;
    }
    if ((entry->attr & 0x0F) == 0x0F) {
        return 0;
    }
    if (entry->attr & 0x08) {
        return 0;
    }
    return 1;
}

static int fat32_entry_is_lfn(const fat32_dir_entry_t *entry) {
    return (entry->attr & 0x0F) == 0x0F;
}

static int fat32_entry_is_file(const fat32_dir_entry_t *entry) {
    return fat32_entry_is_usable(entry) && !(entry->attr & 0x10);
}

static int fat32_entry_is_dir(const fat32_dir_entry_t *entry) {
    return fat32_entry_is_usable(entry) && (entry->attr & 0x10);
}

static uint32_t fat32_entry_cluster(const fat32_dir_entry_t *entry) {
    return ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
}

static void fat32_lfn_append_char(char *lfn, uint32_t pos, uint16_t value) {
    if (pos >= 255) {
        return;
    }
    if (value == 0x0000 || value == 0xFFFF) {
        return;
    }
    lfn[pos] = (value < 128) ? (char)value : '?';
}

static void fat32_lfn_store_entry(const fat32_lfn_entry_t *entry, char *lfn) {
    uint32_t order = entry->order & 0x1F;
    if (order == 0) {
        return;
    }

    uint32_t pos = (order - 1) * 13;
    for (int i = 0; i < 5; i++) {
        fat32_lfn_append_char(lfn, pos++, entry->name1[i]);
    }
    for (int i = 0; i < 6; i++) {
        fat32_lfn_append_char(lfn, pos++, entry->name2[i]);
    }
    for (int i = 0; i < 2; i++) {
        fat32_lfn_append_char(lfn, pos++, entry->name3[i]);
    }
}

static void fat32_entry_display_name(const fat32_dir_entry_t *entry, const char *lfn, int lfn_active, char *out, uint32_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    if (lfn_active && lfn && lfn[0]) {
        strncpy(out, lfn, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    fat32_format_entry_name(entry, out);
}

/**
 * @brief Searches for a directory entry by name within a specific directory cluster.
 * Supports both short names (8.3) and Long File Names (LFN).
 */
static int fat32_find_entry_in_dir(uint32_t dir_cluster, const char *name, fat32_dir_entry_t *out_entry, uint32_t *out_index) {
    char target[11];
    int has_short_target;

    if (!global_fat32_mount || !out_entry || !name) {
        return 0;
    }
    has_short_target = fat32_make_83_name(name, target);

    uint32_t cluster = dir_cluster;
    uint32_t cluster_size = global_fat32_mount->bpb.sectors_per_cluster * 512;
    uint8_t *dir_buf = kmalloc(cluster_size);
    if (!dir_buf) {
        return 0;
    }

    while (fat32_valid_cluster(cluster)) {
        char lfn[256];
        int lfn_active = 0;

        memset(lfn, 0, sizeof(lfn));
        uint32_t lba = fat32_cluster_to_lba(global_fat32_mount, cluster);
        if (vfs_read_device(global_fat32_mount->device, lba * 512, cluster_size, dir_buf) < 0) {
            kfree(dir_buf);
            return 0;
        }

        fat32_dir_entry_t *entry = (fat32_dir_entry_t *)dir_buf;
        for (uint32_t i = 0; i < cluster_size / sizeof(fat32_dir_entry_t); i++) {
            if ((unsigned char)entry[i].name[0] == 0x00) {
                kfree(dir_buf);
                return 0;
            }

            if (fat32_entry_is_lfn(&entry[i])) {
                fat32_lfn_entry_t *lfn_entry = (fat32_lfn_entry_t *)&entry[i];
                if (lfn_entry->order & 0x40) {
                    memset(lfn, 0, sizeof(lfn));
                    lfn_active = 1;
                }
                if (lfn_active) {
                    fat32_lfn_store_entry(lfn_entry, lfn);
                }
                continue;
            }

            if ((unsigned char)entry[i].name[0] == 0xE5) {
                memset(lfn, 0, sizeof(lfn));
                lfn_active = 0;
                continue;
            }
            if (!fat32_entry_is_usable(&entry[i])) {
                memset(lfn, 0, sizeof(lfn));
                lfn_active = 0;
                continue;
            }

            if ((lfn_active && fat32_str_equal_ci(lfn, name)) ||
                (has_short_target && memcmp(entry[i].name, target, 11) == 0)) {
                memcpy(out_entry, &entry[i], sizeof(fat32_dir_entry_t));
                if (out_index) *out_index = i;
                kfree(dir_buf);
                return 1;
            }

            memset(lfn, 0, sizeof(lfn));
            lfn_active = 0;
        }

        cluster = fat32_get_next_cluster(global_fat32_mount, cluster);
    }

    kfree(dir_buf);
    return 0;
}

static int fat32_write_entry_at(uint32_t dir_cluster, uint32_t index, const fat32_dir_entry_t *entry) {
    if (!global_fat32_mount) return 0;
    
    uint32_t cluster_size = global_fat32_mount->bpb.sectors_per_cluster * 512;
    uint8_t *dir_buf = kmalloc(cluster_size);
    if (!dir_buf) return 0;

    uint32_t lba = fat32_cluster_to_lba(global_fat32_mount, dir_cluster);
    vfs_read_device(global_fat32_mount->device, lba * 512, cluster_size, dir_buf);
    
    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)dir_buf;
    memcpy(&entries[index], entry, sizeof(fat32_dir_entry_t));
    
    vfs_write_device(global_fat32_mount->device, lba * 512, cluster_size, dir_buf);
    kfree(dir_buf);
    return 1;
}

static int fat32_find_free_slot_in_dir(uint32_t dir_cluster, uint32_t *out_index) {
    if (!global_fat32_mount) return 0;
    
    uint32_t cluster = dir_cluster;
    uint32_t cluster_size = global_fat32_mount->bpb.sectors_per_cluster * 512;
    uint8_t *dir_buf = kmalloc(cluster_size);
    if (!dir_buf) return 0;

    while (fat32_valid_cluster(cluster)) {
        uint32_t lba = fat32_cluster_to_lba(global_fat32_mount, cluster);
        vfs_read_device(global_fat32_mount->device, lba * 512, cluster_size, dir_buf);
        
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)dir_buf;
        for (uint32_t i = 0; i < cluster_size / sizeof(fat32_dir_entry_t); i++) {
            if ((unsigned char)entries[i].name[0] == 0x00 || (unsigned char)entries[i].name[0] == 0xE5) {
                *out_index = i;
                kfree(dir_buf);
                return 1;
            }
        }
        
        uint32_t next = fat32_get_next_cluster(global_fat32_mount, cluster);
        if (next >= 0x0FFFFFF8) {
            // Should extend directory here if needed, for now just fail if full.
            break;
        }
        cluster = next;
    }
    
    kfree(dir_buf);
    return 0;
}

static int fat32_next_path_component(const char *path, uint32_t *index, char *component, uint32_t component_size) {
    uint32_t pos = 0;
    uint32_t i = *index;

    while (path[i] == '/' || path[i] == '\\') {
        i++;
    }
    if (!path[i]) {
        *index = i;
        return 0;
    }

    while (path[i] && path[i] != '/' && path[i] != '\\') {
        if (pos + 1 >= component_size) {
            return -1;
        }
        component[pos++] = path[i++];
    }
    component[pos] = '\0';
    *index = i;
    return 1;
}

/**
 * @brief Resolves a full path to its corresponding directory entry.
 * @param out_parent_cluster Optional: returns the cluster of the parent directory.
 * @param out_index Optional: returns the index of the entry within the parent directory.
 */
static int fat32_resolve_path(const char *path, fat32_dir_entry_t *out_entry, uint32_t *out_parent_cluster, uint32_t *out_index) {
    uint32_t index = 0;
    uint32_t current_cluster;
    char component[64];
    fat32_dir_entry_t entry;
    int found_any = 0;

    if (!global_fat32_mount || !path || !path[0]) {
        return 0;
    }

    current_cluster = global_fat32_mount->root_cluster;
    while (1) {
        int rc = fat32_next_path_component(path, &index, component, sizeof(component));
        if (rc < 0) {
            return 0;
        }
        if (rc == 0) {
            break;
        }

        uint32_t entry_index = 0;
        if (!fat32_find_entry_in_dir(current_cluster, component, &entry, &entry_index)) {
            return 0;
        }
        found_any = 1;

        uint32_t lookahead = index;
        char next_component[64];
        int has_next = fat32_next_path_component(path, &lookahead, next_component, sizeof(next_component));
        if (has_next < 0) {
            return 0;
        }
        if (has_next == 0) {
            if (out_entry) {
                memcpy(out_entry, &entry, sizeof(fat32_dir_entry_t));
            }
            if (out_parent_cluster) {
                *out_parent_cluster = current_cluster;
            }
            if (out_index) {
                *out_index = entry_index;
            }
            return found_any;
        }

        if (!fat32_entry_is_dir(&entry)) {
            return 0;
        }
        current_cluster = fat32_entry_cluster(&entry);
    }

    return 0;
}

static int fat32_resolve_dir_cluster(const char *path, uint32_t *out_cluster) {
    fat32_dir_entry_t entry;

    if (!global_fat32_mount || !out_cluster) {
        return 0;
    }
    if (!path || !path[0] || strcmp(path, "/") == 0 || strcmp(path, "\\") == 0) {
        *out_cluster = global_fat32_mount->root_cluster;
        return 1;
    }
    if (!fat32_resolve_path(path, &entry, 0, 0) || !fat32_entry_is_dir(&entry)) {
        return 0;
    }

    *out_cluster = fat32_entry_cluster(&entry);
    return 1;
}

static vfs_node_t *fat32_mount(block_device_t *device) {
    fat32_mount_t *mount = kmalloc(sizeof(fat32_mount_t));
    if (!mount) return 0;
    if (vfs_read_device(device, 0, sizeof(fat32_boot_sector_t), (uint8_t *)&mount->bpb) < 0) {
        kfree(mount);
        return 0;
    }
    
    mount->device = device;
    mount->fat_start_lba = mount->bpb.reserved_sectors;
    mount->data_start_lba = mount->fat_start_lba + (mount->bpb.fat_count * mount->bpb.sectors_per_fat_32);
    mount->root_cluster = mount->bpb.root_cluster;
    global_fat32_mount = mount;

    fat32_node_t *root = kmalloc(sizeof(fat32_node_t));
    if (!root) {
        kfree(mount);
        return 0;
    }
    memset(root, 0, sizeof(fat32_node_t));
    strcpy(root->node.name, "fat32");
    root->node.flags = FS_DIRECTORY;
    root->node.read = fat32_read;
    root->node.write = fat32_write;
    root->mount = mount;
    root->first_cluster = mount->root_cluster;
    
    return (vfs_node_t *)root;
}

static filesystem_driver_t fat32_driver = {
    .name = "fat32",
    .probe = fat32_probe,
    .mount = fat32_mount
};

void fat32_register() {
    vfs_register_filesystem(&fat32_driver);
}

int fat32_is_mounted() {
    return global_fat32_mount != 0;
}

int fat32_find_model(uint32_t *start_addr, uint32_t *size) {
    if (!global_fat32_mount) return 0;

    uint8_t *file_buf = (uint8_t *)0x2000000; // 32MB mark, bypassing small heap.
    uint32_t file_size = 0;

    if (!fat32_load_file("model.mklm", file_buf, 16 * 1024 * 1024, &file_size) &&
        !fat32_load_file("models/model.mklm", file_buf, 16 * 1024 * 1024, &file_size)) {
        return 0;
    }

    *start_addr = (uint32_t)file_buf;
    *size = file_size;
    return 1;
}

void fat32_ls_path(const char *path) {
    if (!global_fat32_mount) {
        klog("FAT32 filesystem not mounted.\n");
        return;
    }
    
    uint32_t cluster = 0;
    if (!fat32_resolve_dir_cluster(path, &cluster)) {
        klog("Directory not found.\n");
        return;
    }

    uint32_t cluster_size = global_fat32_mount->bpb.sectors_per_cluster * 512;
    uint8_t *dir_buf = kmalloc(cluster_size);
    if (!dir_buf) return;

    klog("Name        Size\n");
    klog("------------------\n");

    while (fat32_valid_cluster(cluster)) {
        char lfn[256];
        int lfn_active = 0;
        uint32_t lba = fat32_cluster_to_lba(global_fat32_mount, cluster);
        if (vfs_read_device(global_fat32_mount->device, lba * 512, cluster_size, dir_buf) < 0) {
            break;
        }

        memset(lfn, 0, sizeof(lfn));
        fat32_dir_entry_t *entry = (fat32_dir_entry_t *)dir_buf;
        for (uint32_t i = 0; i < cluster_size / sizeof(fat32_dir_entry_t); i++) {
            if ((unsigned char)entry[i].name[0] == 0x00) {
                kfree(dir_buf);
                return;
            }
            if (fat32_entry_is_lfn(&entry[i])) {
                fat32_lfn_entry_t *lfn_entry = (fat32_lfn_entry_t *)&entry[i];
                if (lfn_entry->order & 0x40) {
                    memset(lfn, 0, sizeof(lfn));
                    lfn_active = 1;
                }
                if (lfn_active) {
                    fat32_lfn_store_entry(lfn_entry, lfn);
                }
                continue;
            }
            if (!fat32_entry_is_usable(&entry[i])) {
                memset(lfn, 0, sizeof(lfn));
                lfn_active = 0;
                continue;
            }

            char name[256];
            char size_str[16];
            fat32_entry_display_name(&entry[i], lfn, lfn_active, name, sizeof(name));
            itoa(entry[i].size, size_str, 10);

            klog(name);
            if (fat32_entry_is_dir(&entry[i])) {
                klog("   <DIR>\n");
            } else {
                klog("   ");
                klog(size_str);
                klog(" bytes\n");
            }

            memset(lfn, 0, sizeof(lfn));
            lfn_active = 0;
        }

        cluster = fat32_get_next_cluster(global_fat32_mount, cluster);
    }

    kfree(dir_buf);
}

void fat32_ls() {
    fat32_ls_path("/");
}

int fat32_load_file(const char *filename, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size) {
    fat32_dir_entry_t entry;

    if (!buffer || !out_size) {
        return 0;
    }
    *out_size = 0;

    if (!fat32_resolve_path(filename, &entry, 0, 0) || !fat32_entry_is_file(&entry)) {
        return 0;
    }

    if (entry.size > buffer_size) {
        return 0;
    }

    fat32_node_t file_node;
    memset(&file_node, 0, sizeof(file_node));
    file_node.mount = global_fat32_mount;
    file_node.first_cluster = fat32_entry_cluster(&entry);

    uint32_t cluster = file_node.first_cluster;
    uint32_t total_read = 0;
    uint32_t cluster_size = global_fat32_mount->bpb.sectors_per_cluster * 512;

    klog(" [");
    uint32_t last_pct = 0;

    while (fat32_valid_cluster(cluster) && total_read < entry.size) {
        extern int sys_abort_requested();
        if (sys_abort_requested()) {
            klog("] ABORTED\n");
            return 0;
        }

        uint32_t start_cluster = cluster;
        uint32_t contiguous_clusters = 1;
        uint32_t next = fat32_get_next_cluster(global_fat32_mount, cluster);
        
        // Find how many clusters are contiguous for a burst read
        while (fat32_valid_cluster(next) && next == cluster + 1 && contiguous_clusters < 64) {
            cluster = next;
            next = fat32_get_next_cluster(global_fat32_mount, cluster);
            contiguous_clusters++;
        }

        uint32_t lba = fat32_cluster_to_lba(global_fat32_mount, start_cluster);
        uint32_t bytes_to_read = contiguous_clusters * cluster_size;
        if (total_read + bytes_to_read > entry.size) {
            bytes_to_read = entry.size - total_read;
        }

        // Direct read into target buffer (bypass cluster_buf)
        if (vfs_read_device(global_fat32_mount->device, lba * 512, bytes_to_read, buffer + total_read) < 0) break;
        
        total_read += bytes_to_read;
        cluster = next;

        // Update progress bar
        uint32_t pct = (total_read * 100) / entry.size;
        if (pct >= last_pct + 5) {
            klog("=");
            last_pct = pct;
        }
    }
    klog("> ] 100%\n");

    *out_size = total_read;
    return (total_read == entry.size);
}

uint32_t fat32_get_size(const char *filename) {
    fat32_dir_entry_t entry;
    if (!fat32_resolve_path(filename, &entry, 0, 0) || !fat32_entry_is_file(&entry)) {
        return 0;
    }
    return entry.size;
}

int fat32_get_entry_by_index(const char *path, uint32_t index, char *out_name, int *out_is_dir) {
    if (!global_fat32_mount) return 0;
    
    uint32_t cluster = 0;
    if (!fat32_resolve_dir_cluster(path, &cluster)) return 0;

    uint32_t cluster_size = global_fat32_mount->bpb.sectors_per_cluster * 512;
    uint8_t *dir_buf = kmalloc(cluster_size);
    if (!dir_buf) return 0;

    uint32_t current_idx = 0;
    while (fat32_valid_cluster(cluster)) {
        char lfn[256];
        int lfn_active = 0;
        uint32_t lba = fat32_cluster_to_lba(global_fat32_mount, cluster);
        if (vfs_read_device(global_fat32_mount->device, lba * 512, cluster_size, dir_buf) < 0) break;

        memset(lfn, 0, sizeof(lfn));
        fat32_dir_entry_t *entry = (fat32_dir_entry_t *)dir_buf;
        for (uint32_t i = 0; i < cluster_size / sizeof(fat32_dir_entry_t); i++) {
            if ((unsigned char)entry[i].name[0] == 0x00) {
                kfree(dir_buf);
                return 0;
            }
            if (fat32_entry_is_lfn(&entry[i])) {
                fat32_lfn_entry_t *lfn_entry = (fat32_lfn_entry_t *)&entry[i];
                if (lfn_entry->order & 0x40) {
                    memset(lfn, 0, sizeof(lfn));
                    lfn_active = 1;
                }
                if (lfn_active) {
                    fat32_lfn_store_entry(lfn_entry, lfn);
                }
                continue;
            }
            if (!fat32_entry_is_usable(&entry[i])) {
                memset(lfn, 0, sizeof(lfn));
                lfn_active = 0;
                continue;
            }

            if (current_idx == index) {
                fat32_entry_display_name(&entry[i], lfn, lfn_active, out_name, 64);
                if (out_is_dir) *out_is_dir = fat32_entry_is_dir(&entry[i]);
                kfree(dir_buf);
                return 1;
            }
            current_idx++;
            memset(lfn, 0, sizeof(lfn));
            lfn_active = 0;
        }
        cluster = fat32_get_next_cluster(global_fat32_mount, cluster);
    }

    kfree(dir_buf);
    return 0;
}

int fat32_path_is_dir(const char *path) {
    uint32_t cluster = 0;

    if (!global_fat32_mount) {
        return 0;
    }
    if (!path || !path[0] || strcmp(path, "/") == 0 || strcmp(path, "\\") == 0) {
        return 1;
    }

    return fat32_resolve_dir_cluster(path, &cluster);
}

void fat32_cat(const char *filename) {
    uint32_t file_size = 0;
    uint32_t max_size = 4096;
    uint8_t *file_buf = kmalloc(max_size + 1);

    if (!global_fat32_mount) {
        klog("FAT32 filesystem not mounted.\n");
        return;
    }
    if (!file_buf) {
        klog("Out of memory.\n");
        return;
    }

    if (!fat32_load_file(filename, file_buf, max_size, &file_size)) {
        klog("File not found or too large for cat.\n");
        kfree(file_buf);
        return;
    }

    file_buf[file_size] = '\0';
    klog((char *)file_buf);
    klog("\n");
    kfree(file_buf);
}

int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t size) {
    fat32_dir_entry_t entry;
    uint32_t parent_cluster;
    uint32_t index_in_parent;

    if (!global_fat32_mount || !buffer) {
        return 0;
    }

    if (!fat32_resolve_path(filename, &entry, &parent_cluster, &index_in_parent) || !fat32_entry_is_file(&entry)) {
        return 0;
    }

    fat32_node_t file_node;
    memset(&file_node, 0, sizeof(file_node));
    file_node.mount = global_fat32_mount;
    file_node.first_cluster = fat32_entry_cluster(&entry);

    uint32_t written = fat32_write((vfs_node_t *)&file_node, 0, size, buffer);
    
    // Update directory entry size if it grew
    if (written > entry.size) {
        entry.size = written;
        fat32_write_entry_at(parent_cluster, index_in_parent, &entry);
    }
    
    return written == size;
}

/**
 * @brief Creates a new empty file at the specified path.
 */
int fat32_create_file(const char *filename) {
    char name83[11];
    uint32_t parent_cluster;
    uint32_t index;
    fat32_dir_entry_t entry;

    if (!global_fat32_mount) return 0;
    if (!fat32_make_83_name(filename, name83)) return 0;
    
    // Check if it already exists
    if (fat32_resolve_path(filename, &entry, 0, 0)) return 0;
    
    // Get parent directory
    const char *last_slash = strrchr(filename, '/');
    if (!last_slash) last_slash = strrchr(filename, '\\');
    
    if (last_slash) {
        char parent_path[128];
        uint32_t len = last_slash - filename;
        if (len >= sizeof(parent_path)) return 0;
        if (len == 0) strcpy(parent_path, "/");
        else {
            strncpy(parent_path, filename, len);
            parent_path[len] = '\0';
        }
        if (!fat32_resolve_dir_cluster(parent_path, &parent_cluster)) return 0;
    } else {
        parent_cluster = global_fat32_mount->root_cluster;
    }

    if (!fat32_find_free_slot_in_dir(parent_cluster, &index)) return 0;

    uint32_t first_cluster = fat32_find_free_cluster(global_fat32_mount);
    if (first_cluster == 0xFFFFFFFF) return 0;
    fat32_set_cluster(global_fat32_mount, first_cluster, 0x0FFFFFFF);

    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, name83, 11);
    entry.attr = 0x20; // Archive
    entry.cluster_low = first_cluster & 0xFFFF;
    entry.cluster_high = (first_cluster >> 16) & 0xFFFF;
    entry.size = 0;

    return fat32_write_entry_at(parent_cluster, index, &entry);
}

/**
 * @brief Deletes a file from the filesystem.
 * Frees all clusters in the chain and marks the directory entry as deleted.
 */
int fat32_delete_file(const char *filename) {
    fat32_dir_entry_t entry;
    uint32_t parent_cluster;
    uint32_t index;

    if (!global_fat32_mount) return 0;
    
    if (!fat32_resolve_path(filename, &entry, &parent_cluster, &index)) {
        return 0;
    }

    if (fat32_entry_is_dir(&entry)) {
        // For now, don't delete non-empty directories or directories at all to be safe
        return 0;
    }

    // Free cluster chain
    uint32_t cluster = fat32_entry_cluster(&entry);
    while (fat32_valid_cluster(cluster)) {
        uint32_t next = fat32_get_next_cluster(global_fat32_mount, cluster);
        fat32_set_cluster(global_fat32_mount, cluster, 0); // Mark as free
        cluster = next;
    }

    // Mark directory entry as deleted
    entry.name[0] = 0xE5;
    return fat32_write_entry_at(parent_cluster, index, &entry);
}

/**
 * @brief Creates a new directory at the specified path.
 * Initializes the new directory with '.' and '..' entries.
 */
int fat32_mkdir(const char *path) {
    char name83[11];
    uint32_t parent_cluster;
    uint32_t index;
    fat32_dir_entry_t entry;

    if (!global_fat32_mount) return 0;
    if (!fat32_make_83_name(path, name83)) return 0;
    
    // Check if it already exists
    if (fat32_resolve_path(path, &entry, 0, 0)) return 0;
    
    // Get parent directory
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) last_slash = strrchr(path, '\\');
    
    if (last_slash) {
        char parent_path[128];
        uint32_t len = last_slash - path;
        if (len >= sizeof(parent_path)) return 0;
        if (len == 0) strcpy(parent_path, "/");
        else {
            strncpy(parent_path, path, len);
            parent_path[len] = '\0';
        }
        if (!fat32_resolve_dir_cluster(parent_path, &parent_cluster)) return 0;
    } else {
        parent_cluster = global_fat32_mount->root_cluster;
    }

    if (!fat32_find_free_slot_in_dir(parent_cluster, &index)) return 0;

    uint32_t new_cluster = fat32_find_free_cluster(global_fat32_mount);
    if (new_cluster == 0xFFFFFFFF) return 0;
    fat32_set_cluster(global_fat32_mount, new_cluster, 0x0FFFFFFF);

    // Initialize the new directory with . and ..
    uint32_t cluster_size = global_fat32_mount->bpb.sectors_per_cluster * 512;
    uint8_t *dir_buf = kmalloc(cluster_size);
    if (!dir_buf) {
        fat32_set_cluster(global_fat32_mount, new_cluster, 0);
        return 0;
    }
    memset(dir_buf, 0, cluster_size);

    fat32_dir_entry_t *dot = (fat32_dir_entry_t *)&dir_buf[0];
    fat32_dir_entry_t *dotdot = (fat32_dir_entry_t *)&dir_buf[sizeof(fat32_dir_entry_t)];

    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = 0x10;
    dot->cluster_low = new_cluster & 0xFFFF;
    dot->cluster_high = (new_cluster >> 16) & 0xFFFF;

    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = 0x10;
    dotdot->cluster_low = parent_cluster & 0xFFFF;
    dotdot->cluster_high = (parent_cluster >> 16) & 0xFFFF;

    uint32_t lba = fat32_cluster_to_lba(global_fat32_mount, new_cluster);
    vfs_write_device(global_fat32_mount->device, lba * 512, cluster_size, dir_buf);
    kfree(dir_buf);

    // Create entry in parent
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.name, name83, 11);
    entry.attr = 0x10; // Directory
    entry.cluster_low = new_cluster & 0xFFFF;
    entry.cluster_high = (new_cluster >> 16) & 0xFFFF;
    entry.size = 0;

    return fat32_write_entry_at(parent_cluster, index, &entry);
}
