#include "extfs.h"
#include "kheap.h"
#include "video.h"
#include <string.h>

#define EXT_SUPERBLOCK_OFFSET 1024
#define EXT_SUPERBLOCK_SIZE   1024
#define EXT_MAGIC             0xEF53
#define EXT_ROOT_INO          2
#define EXT_FT_REG_FILE       1
#define EXT_FT_DIR            2
#define EXT_PATH_MAX          64

typedef struct {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t reserved_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
} __attribute__((packed)) ext_superblock_t;

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t reserved[12];
} __attribute__((packed)) ext_group_desc_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];
} __attribute__((packed)) ext_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
} __attribute__((packed)) ext_dir_entry_t;

typedef struct {
    block_device_t *device;
    ext_superblock_t superblock;
    uint32_t block_size;
    uint32_t inode_size;
    const char *variant;
} ext_mount_t;

static ext_mount_t *global_ext_mount = 0;

static uint16_t read_le16(uint16_t value) {
    return value;
}

static uint32_t read_le32(uint32_t value) {
    return value;
}

static int ext_read_superblock(block_device_t *device, ext_superblock_t *out) {
    if (vfs_read_device(device, EXT_SUPERBLOCK_OFFSET, sizeof(ext_superblock_t), (uint8_t *)out) < 0) {
        return -1;
    }
    return read_le16(out->magic) == EXT_MAGIC ? 0 : -1;
}

static uint32_t ext_block_offset(ext_mount_t *mount, uint32_t block) {
    return block * mount->block_size;
}

static int ext_read_block(ext_mount_t *mount, uint32_t block, uint8_t *buffer) {
    return vfs_read_device(mount->device, ext_block_offset(mount, block), mount->block_size, buffer) == (int)mount->block_size ? 0 : -1;
}

static int ext_read_group0(ext_mount_t *mount) {
    uint32_t gd_offset = mount->block_size == 1024 ? 2048 : mount->block_size;
    ext_group_desc_t group;
    return vfs_read_device(mount->device, gd_offset, sizeof(ext_group_desc_t), (uint8_t *)&group) == sizeof(ext_group_desc_t) ? 0 : -1;
}

static int ext_read_group_desc(ext_mount_t *mount, uint32_t group, ext_group_desc_t *out) {
    if (!mount || !out) {
        return -1;
    }

    uint32_t gd_table = mount->block_size == 1024 ? 2048 : mount->block_size;
    uint32_t offset = gd_table + (group * sizeof(ext_group_desc_t));
    return vfs_read_device(mount->device, offset, sizeof(ext_group_desc_t), (uint8_t *)out) == sizeof(ext_group_desc_t) ? 0 : -1;
}

static int ext_read_inode(ext_mount_t *mount, uint32_t inode_num, ext_inode_t *out) {
    if (!mount || !out || inode_num == 0) {
        return -1;
    }

    uint32_t inodes_per_group = read_le32(mount->superblock.inodes_per_group);
    if (inodes_per_group == 0) {
        return -1;
    }

    uint32_t index = inode_num - 1;
    uint32_t group = index / inodes_per_group;
    uint32_t local_index = index % inodes_per_group;
    ext_group_desc_t desc;
    if (ext_read_group_desc(mount, group, &desc) != 0) {
        return -1;
    }

    uint32_t table_offset = ext_block_offset(mount, read_le32(desc.inode_table));
    uint32_t inode_offset = table_offset + (local_index * mount->inode_size);
    return vfs_read_device(mount->device, inode_offset, sizeof(ext_inode_t), (uint8_t *)out) == sizeof(ext_inode_t) ? 0 : -1;
}

static int ext_name_equals(const char *name, const char *entry_name, uint8_t entry_len) {
    uint32_t len = strlen(name);
    if (len != entry_len) {
        return 0;
    }
    return memcmp(name, entry_name, entry_len) == 0;
}

static uint32_t ext_inode_block(ext_mount_t *mount, ext_inode_t *inode, uint32_t logical_block, uint8_t *scratch) {
    uint32_t entries = mount->block_size / sizeof(uint32_t);

    if (logical_block < 12) {
        return read_le32(inode->block[logical_block]);
    }

    logical_block -= 12;
    if (logical_block < entries) {
        if (inode->block[12] == 0) {
            return 0;
        }
        if (ext_read_block(mount, read_le32(inode->block[12]), scratch) != 0) {
            return 0;
        }
        uint32_t *blocks = (uint32_t *)scratch;
        return read_le32(blocks[logical_block]);
    }

    logical_block -= entries;
    uint32_t double_span = entries * entries;
    if (logical_block < double_span) {
        if (inode->block[13] == 0) {
            return 0;
        }
        uint32_t first_index = logical_block / entries;
        uint32_t second_index = logical_block % entries;
        if (ext_read_block(mount, read_le32(inode->block[13]), scratch) != 0) {
            return 0;
        }
        uint32_t first_block = read_le32(((uint32_t *)scratch)[first_index]);
        if (!first_block || ext_read_block(mount, first_block, scratch) != 0) {
            return 0;
        }
        return read_le32(((uint32_t *)scratch)[second_index]);
    }

    logical_block -= double_span;
    if (inode->block[14] == 0) {
        return 0;
    }
    uint32_t first_index = logical_block / double_span;
    uint32_t rest = logical_block % double_span;
    uint32_t second_index = rest / entries;
    uint32_t third_index = rest % entries;
    if (first_index >= entries) {
        return 0;
    }
    if (ext_read_block(mount, read_le32(inode->block[14]), scratch) != 0) {
        return 0;
    }
    uint32_t first_block = read_le32(((uint32_t *)scratch)[first_index]);
    if (!first_block || ext_read_block(mount, first_block, scratch) != 0) {
        return 0;
    }
    uint32_t second_block = read_le32(((uint32_t *)scratch)[second_index]);
    if (!second_block || ext_read_block(mount, second_block, scratch) != 0) {
        return 0;
    }
    return read_le32(((uint32_t *)scratch)[third_index]);
}

static int ext_find_entry_in_dir(ext_mount_t *mount, ext_inode_t *dir, const char *filename, ext_inode_t *out_inode, uint8_t *out_type, uint32_t *out_size) {
    uint8_t *block_buf;
    uint8_t *indirect_buf;

    if (!mount || !dir || !filename || !out_inode) {
        return 0;
    }

    block_buf = (uint8_t *)kmalloc(mount->block_size);
    if (!block_buf) {
        return 0;
    }
    indirect_buf = (uint8_t *)kmalloc(mount->block_size);
    if (!indirect_buf) {
        kfree(block_buf);
        return 0;
    }

    uint32_t dir_blocks = (read_le32(dir->size) + mount->block_size - 1) / mount->block_size;
    for (uint32_t b = 0; b < dir_blocks; b++) {
        uint32_t phys_block = ext_inode_block(mount, dir, b, indirect_buf);
        if (!phys_block || ext_read_block(mount, phys_block, block_buf) != 0) {
            continue;
        }

        uint32_t pos = 0;
        while (pos + sizeof(ext_dir_entry_t) <= mount->block_size) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(block_buf + pos);
            if (entry->rec_len == 0) {
                break;
            }
            if (entry->inode && entry->name_len && ext_name_equals(filename, (char *)(entry + 1), entry->name_len)) {
                if (ext_read_inode(mount, read_le32(entry->inode), out_inode) == 0) {
                    if (out_type) {
                        *out_type = entry->file_type;
                    }
                    if (out_size) {
                        *out_size = read_le32(out_inode->size);
                    }
                    kfree(indirect_buf);
                    kfree(block_buf);
                    return 1;
                }
            }
            pos += entry->rec_len;
        }
    }

    kfree(indirect_buf);
    kfree(block_buf);
    return 0;
}

static int ext_next_path_component(const char *path, uint32_t *index, char *component, uint32_t component_size) {
    uint32_t pos = 0;
    uint32_t i = *index;

    while (path[i] == '/') {
        i++;
    }
    if (!path[i]) {
        *index = i;
        return 0;
    }

    while (path[i] && path[i] != '/') {
        if (pos + 1 >= component_size) {
            return -1;
        }
        component[pos++] = path[i++];
    }
    component[pos] = '\0';
    *index = i;
    return 1;
}

static int ext_resolve_path(ext_mount_t *mount, const char *path, ext_inode_t *out_inode, uint8_t *out_type, uint32_t *out_size) {
    ext_inode_t current;
    ext_inode_t next;
    uint32_t index = 0;
    char component[EXT_PATH_MAX];
    uint8_t type = EXT_FT_DIR;

    if (!mount || !path || !path[0] || !out_inode) {
        return 0;
    }
    if (ext_read_inode(mount, EXT_ROOT_INO, &current) != 0) {
        return 0;
    }

    while (1) {
        int rc = ext_next_path_component(path, &index, component, sizeof(component));
        if (rc < 0) {
            return 0;
        }
        if (rc == 0) {
            memcpy(out_inode, &current, sizeof(ext_inode_t));
            if (out_type) *out_type = type;
            if (out_size) *out_size = read_le32(current.size);
            return 1;
        }

        if (type != EXT_FT_DIR) {
            return 0;
        }
        if (!ext_find_entry_in_dir(mount, &current, component, &next, &type, out_size)) {
            return 0;
        }
        memcpy(&current, &next, sizeof(ext_inode_t));
    }
}

static const char *ext_variant(ext_superblock_t *superblock) {
    uint32_t compat = read_le32(superblock->feature_compat);
    uint32_t incompat = read_le32(superblock->feature_incompat);
    uint32_t ro_compat = read_le32(superblock->feature_ro_compat);

    if (incompat & 0x40) {
        return "ext4";
    }
    if (compat & 0x4) {
        return "ext3";
    }
    if (ro_compat || incompat || compat) {
        return "ext2+";
    }
    return "ext2";
}

static int ext_probe(block_device_t *device) {
    ext_superblock_t superblock;
    return ext_read_superblock(device, &superblock) == 0;
}

static vfs_node_t *ext_mount(block_device_t *device) {
    ext_mount_t *mount = (ext_mount_t *)kmalloc(sizeof(ext_mount_t));
    if (!mount) return 0;
    memset(mount, 0, sizeof(ext_mount_t));

    if (ext_read_superblock(device, &mount->superblock) != 0) {
        kfree(mount);
        return 0;
    }

    mount->device = device;
    mount->block_size = 1024 << read_le32(mount->superblock.log_block_size);
    mount->inode_size = mount->superblock.inode_size ? read_le16(mount->superblock.inode_size) : 128;
    mount->variant = ext_variant(&mount->superblock);
    if (ext_read_group0(mount) != 0) {
        kfree(mount);
        return 0;
    }
    global_ext_mount = mount;

    vfs_node_t *root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!root) {
        kfree(mount);
        return 0;
    }
    memset(root, 0, sizeof(vfs_node_t));
    strcpy(root->name, mount->variant);
    root->flags = FS_DIRECTORY;
    root->ptr = (vfs_node_t *)mount;
    return root;
}

static filesystem_driver_t ext_driver = {
    .name = "ext",
    .probe = ext_probe,
    .mount = ext_mount
};

void extfs_register() {
    vfs_register_filesystem(&ext_driver);
}

void extfs_ls() {
    if (!global_ext_mount) {
        klog("EXT filesystem not mounted.\n");
        return;
    }

    ext_inode_t root;
    uint8_t *block_buf;
    if (ext_read_inode(global_ext_mount, EXT_ROOT_INO, &root) != 0) {
        klog("EXT: failed to read root inode.\n");
        return;
    }

    block_buf = (uint8_t *)kmalloc(global_ext_mount->block_size);
    if (!block_buf) {
        return;
    }

    klog("Name        Size\n");
    klog("------------------\n");
    uint8_t *indirect_buf = (uint8_t *)kmalloc(global_ext_mount->block_size);
    if (!indirect_buf) {
        kfree(block_buf);
        return;
    }

    uint32_t dir_blocks = (read_le32(root.size) + global_ext_mount->block_size - 1) / global_ext_mount->block_size;
    for (uint32_t b = 0; b < dir_blocks; b++) {
        uint32_t phys_block = ext_inode_block(global_ext_mount, &root, b, indirect_buf);
        if (!phys_block || ext_read_block(global_ext_mount, phys_block, block_buf) != 0) {
            continue;
        }

        uint32_t pos = 0;
        while (pos + sizeof(ext_dir_entry_t) <= global_ext_mount->block_size) {
            ext_dir_entry_t *entry = (ext_dir_entry_t *)(block_buf + pos);
            if (entry->rec_len == 0) {
                break;
            }
            if (entry->inode && entry->name_len) {
                char name[64];
                uint8_t len = entry->name_len;
                if (len > 63) len = 63;
                memcpy(name, (char *)(entry + 1), len);
                name[len] = '\0';
                klog(name);
                if (entry->file_type == EXT_FT_DIR) {
                    klog("   <DIR>\n");
                } else {
                    ext_inode_t inode;
                    char size_str[16];
                    if (ext_read_inode(global_ext_mount, read_le32(entry->inode), &inode) == 0) {
                        itoa(read_le32(inode.size), size_str, 10);
                        klog("   ");
                        klog(size_str);
                        klog(" bytes\n");
                    } else {
                        klog("   ? bytes\n");
                    }
                }
            }
            pos += entry->rec_len;
        }
    }

    kfree(indirect_buf);
    kfree(block_buf);
}

void extfs_cat(const char *filename) {
    uint32_t size = 0;
    uint32_t max_size = 4096;
    uint8_t *file_buf;

    if (!global_ext_mount) {
        klog("EXT filesystem not mounted.\n");
        return;
    }
    file_buf = (uint8_t *)kmalloc(max_size + 1);
    if (!file_buf) {
        return;
    }
    if (!extfs_load_file(filename, file_buf, max_size, &size)) {
        klog("EXT: file not found or too large.\n");
        kfree(file_buf);
        return;
    }

    file_buf[size] = '\0';
    klog((char *)file_buf);
    klog("\n");
    kfree(file_buf);
}

int extfs_load_file(const char *filename, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size) {
    ext_inode_t inode;
    uint8_t type = 0;
    uint32_t size = 0;
    uint8_t *block_buf;
    uint8_t *indirect_buf;
    uint32_t copied = 0;

    if (!global_ext_mount || !filename || !buffer || !out_size) {
        return 0;
    }
    *out_size = 0;

    if (!ext_resolve_path(global_ext_mount, filename, &inode, &type, &size) || type == EXT_FT_DIR) {
        return 0;
    }
    if (size > buffer_size) {
        return 0;
    }

    block_buf = (uint8_t *)kmalloc(global_ext_mount->block_size);
    if (!block_buf) {
        return 0;
    }
    indirect_buf = (uint8_t *)kmalloc(global_ext_mount->block_size);
    if (!indirect_buf) {
        kfree(block_buf);
        return 0;
    }

    uint32_t logical_blocks = (size + global_ext_mount->block_size - 1) / global_ext_mount->block_size;
    for (uint32_t b = 0; b < logical_blocks && copied < size; b++) {
        uint32_t phys_block = ext_inode_block(global_ext_mount, &inode, b, indirect_buf);
        if (!phys_block || ext_read_block(global_ext_mount, phys_block, block_buf) != 0) {
            break;
        }
        uint32_t chunk = size - copied;
        if (chunk > global_ext_mount->block_size) {
            chunk = global_ext_mount->block_size;
        }
        memcpy(buffer + copied, block_buf, chunk);
        copied += chunk;
    }

    kfree(indirect_buf);
    kfree(block_buf);
    if (copied != size) {
        return 0;
    }

    *out_size = size;
    return 1;
}
