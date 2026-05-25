#include <stdint.h>
#include <string.h>
#include "math.h"
#include "pmm.h"
#include "kheap.h"

enum {
    SYS_PRINT = 1,
    SYS_YIELD = 2,
    SYS_GET_CHAR = 4,
    SYS_LLM_QUERY = 5,
    SYS_PUT_CHAR = 8,
    SYS_CLEAR = 9,
    SYS_MEM_STATS = 10,
    SYS_HEAP_STATS = 11,
    SYS_HEAP_TEST = 12,
    SYS_FAT_LS = 13,
    SYS_FAT_CAT = 14,
    SYS_EXT_LS = 15,
    SYS_EXT_CAT = 16,
    SYS_LLM_LOAD = 17,
    SYS_LLM_STATUS_STR = 18,
    SYS_LLM_INFO_STR = 19,
    SYS_FAT_CREATE = 21,
    SYS_FAT_WRITE = 22,
    SYS_FAT_DELETE = 23,
    SYS_FAT_MKDIR = 24,
    SYS_NET_STATUS_STR = 25,
    SYS_NET_CONFIG_DHCP = 26,
    SYS_NET_CONFIG_STATIC = 27,
    SYS_MEM_REGION_COUNT = 28,
    SYS_MEM_REGION_GET = 29,
    SYS_GPU_STATUS_STR = 30,
    SYS_FAT_GET_SIZE = 31,
    SYS_FAT_READ_FILE = 32,
    SYS_FAT_GET_ENTRY = 33,
    SYS_PMM_ALLOC_REGION = 34,
    SYS_LLM_LOAD_FILE = 35,
    SYS_FAT_IS_DIR = 36,
    SYS_LLM_TRACE_SET = 37,
    SYS_LLM_TRACE_STATUS = 38,
    SYS_ARP_STATUS_STR = 39,
    SYS_HIGH_MEM_TEST = 40,
    SYS_LLM_NET_SET = 41,
    SYS_LLM_NET_STATUS_STR = 42,
    SYS_NET_POLL = 43,
    SYS_LLM_NET_PORT = 44
};

static inline void syscall_print(const char *msg) {
    asm volatile ("int $0x80" : : "a"(SYS_PRINT), "c"(msg) : "memory");
}

static inline void syscall_yield(void) {
    asm volatile ("int $0x80" : : "a"(SYS_YIELD) : "memory");
}

static inline char syscall_get_char(void) {
    char c = 0;
    asm volatile ("int $0x80" : : "a"(SYS_GET_CHAR), "c"(&c) : "memory");
    return c;
}

static inline void syscall_llm_query(const char *prompt, char *response) {
    asm volatile ("int $0x80" : : "a"(SYS_LLM_QUERY), "c"(prompt), "d"(response) : "memory");
}

static inline void syscall_put_char(char c) {
    asm volatile ("int $0x80" : : "a"(SYS_PUT_CHAR), "c"((uint32_t)c) : "memory");
}

static inline void syscall_clear(void) {
    asm volatile ("int $0x80" : : "a"(SYS_CLEAR) : "memory");
}

static inline void syscall_mem_stats(pmm_stats_t *stats) {
    asm volatile ("int $0x80" : : "a"(SYS_MEM_STATS), "c"(stats) : "memory");
}

static inline void syscall_heap_stats(kheap_stats_t *stats) {
    asm volatile ("int $0x80" : : "a"(SYS_HEAP_STATS), "c"(stats) : "memory");
}

static inline int syscall_heap_test(void) {
    int result = 0;
    asm volatile ("int $0x80" : : "a"(SYS_HEAP_TEST), "c"(&result) : "memory");
    return result;
}

static inline void syscall_fat_ls(const char *path) {
    asm volatile ("int $0x80" : : "a"(SYS_FAT_LS), "c"(path) : "memory");
}

static inline void syscall_fat_cat(const char *path) {
    asm volatile ("int $0x80" : : "a"(SYS_FAT_CAT), "c"(path) : "memory");
}

static inline void syscall_ext_ls(void) {
    asm volatile ("int $0x80" : : "a"(SYS_EXT_LS) : "memory");
}

static inline void syscall_ext_cat(const char *path) {
    asm volatile ("int $0x80" : : "a"(SYS_EXT_CAT), "c"(path) : "memory");
}

static inline int syscall_llm_load(uint32_t addr, uint32_t size, const char *name) {
    int result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_LLM_LOAD), "c"(addr), "d"(size), "b"(name) : "memory");
    return result;
}

static inline uint32_t syscall_fat_get_size(const char *path) {
    uint32_t result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_FAT_GET_SIZE), "c"(path) : "memory");
    return result;
}

static inline int syscall_fat_read_file(const char *path, uint8_t *buffer, uint32_t size) {
    int result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_FAT_READ_FILE), "c"(path), "d"(buffer), "b"(size) : "memory");
    return result;
}

static inline int syscall_fat_get_entry(const char *path, uint32_t index, char *name) {
    int result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_FAT_GET_ENTRY), "c"(path), "d"(index), "b"(name) : "memory");
    return result;
}

static inline uint32_t syscall_pmm_alloc_region(uint32_t size) {
    uint32_t result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_PMM_ALLOC_REGION), "c"(size) : "memory");
    return result;
}

static inline int syscall_llm_load_file(const char *path) {
    int result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_LLM_LOAD_FILE), "c"(path) : "memory");
    return result;
}

static inline int syscall_fat_is_dir(const char *path) {
    int result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_FAT_IS_DIR), "c"(path) : "memory");
    return result;
}

static inline void syscall_llm_trace_set(int enabled) {
    asm volatile ("int $0x80" : : "a"(SYS_LLM_TRACE_SET), "c"((uint32_t)enabled) : "memory");
}

static inline int syscall_llm_trace_status(void) {
    int result;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_LLM_TRACE_STATUS) : "memory");
    return result;
}

static inline void syscall_llm_status_str(char *buf) {
    asm volatile ("int $0x80" : : "a"(SYS_LLM_STATUS_STR), "c"(buf) : "memory");
}

static inline void syscall_llm_info_str(char *buf) {
    asm volatile ("int $0x80" : : "a"(SYS_LLM_INFO_STR), "c"(buf) : "memory");
}

static inline int syscall_fat_create(const char *path) {
    int result = 0;
    asm volatile ("int $0x80" : : "a"(SYS_FAT_CREATE), "c"(path), "d"(&result) : "memory");
    return result;
}

static inline void syscall_fat_write(const char *path, const char *data, uint32_t size) {
    asm volatile ("int $0x80" : : "a"(SYS_FAT_WRITE), "c"(path), "d"(data), "b"(size) : "memory");
}

static inline int syscall_fat_delete(const char *path) {
    int result = 0;
    asm volatile ("int $0x80" : : "a"(SYS_FAT_DELETE), "c"(path), "d"(&result) : "memory");
    return result;
}

static inline int syscall_fat_mkdir(const char *path) {
    int result = 0;
    asm volatile ("int $0x80" : : "a"(SYS_FAT_MKDIR), "c"(path), "d"(&result) : "memory");
    return result;
}

static inline void syscall_net_status_str(char *buf) {
    asm volatile ("int $0x80" : : "a"(SYS_NET_STATUS_STR), "c"(buf) : "memory");
}

static inline int syscall_net_config_dhcp(void) {
    int result = 0;
    asm volatile ("int $0x80" : : "a"(SYS_NET_CONFIG_DHCP), "c"(&result) : "memory");
    return result;
}

static inline int syscall_net_config_static(const char *ip, const char *mask, const char *gw) {
    const char *args[3];
    int result = 0;
    args[0] = ip;
    args[1] = mask;
    args[2] = gw;
    asm volatile ("int $0x80" : : "a"(SYS_NET_CONFIG_STATIC), "c"(args), "d"(&result), "b"(0) : "memory");
    return result;
}

static inline uint32_t syscall_mem_region_count(void) {
    uint32_t count = 0;
    asm volatile ("int $0x80" : : "a"(SYS_MEM_REGION_COUNT), "c"(&count) : "memory");
    return count;
}

static inline int syscall_mem_region_get(uint32_t index, pmm_memory_region_t *region) {
    int result = 0;
    asm volatile ("int $0x80" : : "a"(SYS_MEM_REGION_GET), "c"(index), "d"(region), "b"(&result) : "memory");
    return result;
}

static inline void syscall_gpu_status_str(char *buf) {
    asm volatile ("int $0x80" : : "a"(SYS_GPU_STATUS_STR), "c"(buf) : "memory");
}

static inline void syscall_arp_status_str(char *buf) {
    asm volatile ("int $0x80" : : "a"(SYS_ARP_STATUS_STR), "c"(buf) : "memory");
}

static inline int syscall_highmem_test(void) {
    int result = 0;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_HIGH_MEM_TEST) : "memory");
    return result;
}

static inline int syscall_llm_net_set(int enabled) {
    int result = 0;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_LLM_NET_SET), "c"((uint32_t)enabled) : "memory");
    return result;
}

static inline void syscall_llm_net_status_str(char *buf) {
    asm volatile ("int $0x80" : : "a"(SYS_LLM_NET_STATUS_STR), "c"(buf) : "memory");
}

static inline void syscall_net_poll(void) {
    asm volatile ("int $0x80" : : "a"(SYS_NET_POLL) : "memory");
}

static inline int syscall_llm_net_port(uint16_t port) {
    int result = 0;
    asm volatile ("int $0x80" : "=a"(result) : "a"(SYS_LLM_NET_PORT), "c"((uint32_t)port) : "memory");
    return result;
}

static char shell_cwd[64] = "";

static void shell_print_uint(uint32_t value) {
    char buf[11];
    int pos = 10;

    buf[pos] = '\0';
    if (value == 0) {
        syscall_put_char('0');
        return;
    }

    while (value > 0 && pos > 0) {
        buf[--pos] = '0' + (value % 10);
        value /= 10;
    }

    syscall_print(&buf[pos]);
}

static void shell_print_hex32(uint32_t value) {
    const char *hex = "0123456789ABCDEF";

    syscall_print("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        syscall_put_char(hex[(value >> shift) & 0xF]);
    }
}

static void shell_print_kib_line(const char *label, uint32_t bytes) {
    syscall_print(label);
    shell_print_uint(bytes / 1024);
    syscall_print(" KiB\n");
}

static void shell_print_kib_value_line(const char *label, uint32_t kib) {
    syscall_print(label);
    shell_print_uint(kib);
    syscall_print(" KiB\n");
}

static int shell_resolve_path(const char *input, char *out, uint32_t out_size) {
    char combined[128];
    char segment[64];
    uint32_t combined_len = 0;
    uint32_t index = 0;
    uint32_t out_len = 0;
    uint32_t seg_len = 0;

    if (!input || !out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    if (input[0] == '/' || input[0] == '\\') {
        strncpy(combined, input, sizeof(combined) - 1);
        combined[sizeof(combined) - 1] = '\0';
    } else if (shell_cwd[0]) {
        strncpy(combined, shell_cwd, sizeof(combined) - 1);
        combined[sizeof(combined) - 1] = '\0';
        combined_len = strlen(combined);
        if (combined_len + 1 >= sizeof(combined)) {
            return 0;
        }
        combined[combined_len++] = '/';
        combined[combined_len] = '\0';
        if (combined_len + strlen(input) >= sizeof(combined)) {
            return 0;
        }
        strcat(combined, input);
    } else {
        strncpy(combined, input, sizeof(combined) - 1);
        combined[sizeof(combined) - 1] = '\0';
    }

    while (combined[index]) {
        while (combined[index] == '/' || combined[index] == '\\') {
            index++;
        }
        if (!combined[index]) {
            break;
        }

        seg_len = 0;
        while (combined[index] && combined[index] != '/' && combined[index] != '\\') {
            if (seg_len + 1 >= sizeof(segment)) {
                return 0;
            }
            segment[seg_len++] = combined[index++];
        }
        segment[seg_len] = '\0';

        if (strcmp(segment, ".") == 0) {
            continue;
        }
        if (strcmp(segment, "..") == 0) {
            while (out_len > 0 && out[out_len - 1] != '/') {
                out[--out_len] = '\0';
            }
            if (out_len > 0 && out[out_len - 1] == '/') {
                out[--out_len] = '\0';
            }
            continue;
        }

        if (out_len > 0) {
            if (out_len + 1 >= out_size) {
                return 0;
            }
            out[out_len++] = '/';
            out[out_len] = '\0';
        }
        if (out_len + seg_len >= out_size) {
            return 0;
        }
        memcpy(out + out_len, segment, seg_len);
        out_len += seg_len;
        out[out_len] = '\0';
    }

    return 1;
}

static void shell_llm_selftest(void) {
    char response[128];
    const char *tests[] = {
        "hola",
        "estado",
        "kernel",
        "ayuda",
        "que estado tiene el kernel"
    };

    for (int i = 0; i < 5; i++) {
        syscall_print("> ");
        syscall_print(tests[i]);
        syscall_put_char('\n');
        syscall_llm_query(tests[i], response);
        syscall_print(response);
        syscall_put_char('\n');
    }
}

static void shell_mem(void) {
    pmm_stats_t stats;
    kheap_stats_t heap;
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;

    memset(&stats, 0, sizeof(stats));
    memset(&heap, 0, sizeof(heap));
    syscall_mem_stats(&stats);
    syscall_heap_stats(&heap);

    total_bytes = stats.total_blocks * stats.page_size;
    used_bytes = stats.used_blocks * stats.page_size;
    free_bytes = stats.free_blocks * stats.page_size;

    syscall_print("Memory:\n");
    shell_print_kib_line("  addressable: ", stats.addressable_bytes ? stats.addressable_bytes : total_bytes);
    shell_print_kib_line("  mmap usable: ", stats.mmap_usable_bytes ? stats.mmap_usable_bytes : total_bytes);
    shell_print_kib_line("  mmap reserved: ", stats.mmap_reserved_bytes);
    shell_print_kib_line("  managed used: ", used_bytes);
    shell_print_kib_line("  managed free: ", free_bytes);
    shell_print_kib_line("  bitmap:       ", stats.bitmap_size);
    syscall_print("  mmap regions: ");
    shell_print_uint(stats.mmap_region_count);
    syscall_print("\n");
    syscall_print("  mmap invalid: ");
    shell_print_uint(stats.mmap_invalid_regions);
    syscall_print("\n");
    syscall_print("  mmap overlaps: ");
    shell_print_uint(stats.mmap_overlap_count);
    syscall_print("\n");
    syscall_print("  high regions: ");
    shell_print_uint(stats.mmap_high_region_count);
    syscall_print("\n");
    shell_print_kib_value_line("  high usable: ", stats.mmap_high_usable_kib);
    shell_print_kib_value_line("  high reserved: ", stats.mmap_high_reserved_kib);
    syscall_print("  low pool blocks: used ");
    shell_print_uint(stats.low_pool_used_blocks);
    syscall_print(" / total ");
    shell_print_uint(stats.low_pool_total_blocks);
    syscall_print("\n");
    syscall_print("  high pool blocks: used ");
    shell_print_uint(stats.high_pool_used_blocks);
    syscall_print(" / total ");
    shell_print_uint(stats.high_pool_total_blocks);
    syscall_print("\n");
    syscall_print("  high pool allocatable: ");
    syscall_print(stats.high_pool_allocatable ? "yes\n" : "no\n");
    syscall_print("  blocks: used ");
    shell_print_uint(stats.used_blocks);
    syscall_print(" / total ");
    shell_print_uint(stats.total_blocks);
    syscall_print("\n");

    syscall_print("Boot reservations:\n");
    shell_print_kib_line("  low+kernel+bitmap: ", stats.low_reserved_bytes);
    shell_print_kib_line("  bitmap bytes:      ", stats.bitmap_size);
    syscall_print("  bitmap addr:       ");
    shell_print_uint(stats.bitmap_start);
    syscall_print("\n");
    syscall_print("  modules:           ");
    shell_print_uint(stats.module_count);
    syscall_print("\n");
    shell_print_kib_line("  module bytes:      ", stats.module_reserved_bytes);

    syscall_print("Kernel heap:\n");
    shell_print_kib_line("  heap size: ", heap.size);
    shell_print_kib_line("  heap used: ", heap.used_bytes);
    shell_print_kib_line("  heap free: ", heap.free_bytes);
    syscall_print("  blocks: used ");
    shell_print_uint(heap.blocks - heap.free_blocks);
    syscall_print(" / total ");
    shell_print_uint(heap.blocks);
    syscall_print("\n");
}

static const char *shell_memory_type_name(uint32_t type) {
    if (type == 1) {
        return "usable";
    }
    if (type == 2) {
        return "reserved";
    }
    if (type == 3) {
        return "acpi";
    }
    if (type == 4) {
        return "nvs";
    }
    if (type == 5) {
        return "badram";
    }
    return "other";
}

static void shell_mem_map(void) {
    uint32_t count = syscall_mem_region_count();
    pmm_memory_region_t region;

    syscall_print("Memory map:\n");
    if (count == 0) {
        syscall_print("  no memory map regions recorded\n");
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        memset(&region, 0, sizeof(region));
        if (!syscall_mem_region_get(i, &region)) {
            continue;
        }

        syscall_print("  ");
        shell_print_uint(i);
        syscall_print(": base=");
        shell_print_hex32(region.base);
        syscall_print(" size=");
        shell_print_uint(region.size / 1024);
        syscall_print(" KiB type=");
        syscall_print(shell_memory_type_name(region.type));
        syscall_print(" (");
        shell_print_uint(region.type);
        syscall_print(")\n");
    }
}

static void shell_gpu_info(void) {
    char buf[768];

    syscall_gpu_status_str(buf);
    syscall_print(buf);
}

static void shell_arp_info(void) {
    char buf[512];
    syscall_arp_status_str(buf);
    syscall_print(buf);
}

static void shell_run_llm_query(const char *prompt) {
    char response[128];

    syscall_print("LLM is thinking...\n");
    syscall_llm_query(prompt, response);
    syscall_print(response);
    syscall_put_char('\n');
}

static void shell_llm_trace_command(const char *args) {
    if (strcmp(args, "on") == 0) {
        syscall_llm_trace_set(1);
        syscall_print("LLM trace enabled.\n");
    } else if (strcmp(args, "off") == 0) {
        syscall_llm_trace_set(0);
        syscall_print("LLM trace disabled.\n");
    } else if (strcmp(args, "status") == 0) {
        syscall_print("LLM trace: ");
        syscall_print(syscall_llm_trace_status() ? "on\n" : "off\n");
    } else {
        syscall_print("Usage: llm trace on|off|status\n");
    }
}

static void shell_llm_net_command(const char *args) {
    char buf[256];

    if (strcmp(args, "on") == 0) {
        syscall_llm_net_set(1);
        syscall_print("LLM net service enabled.\n");
    } else if (strcmp(args, "off") == 0) {
        syscall_llm_net_set(0);
        syscall_print("LLM net service disabled.\n");
    } else if (strcmp(args, "status") == 0) {
        syscall_llm_net_status_str(buf);
        syscall_print(buf);
    } else if (strstr(args, "port ") == args) {
        uint32_t value = 0;
        const char *p = args + 5;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (uint32_t)(*p - '0');
            p++;
        }
        if (*p || value == 0 || value > 65535 || !syscall_llm_net_port((uint16_t)value)) {
            syscall_print("Invalid UDP port.\n");
        } else {
            syscall_print("LLM net UDP port updated.\n");
        }
    } else {
        syscall_print("Usage: llm net on|off|status|port <1-65535>\n");
    }
}

static void shell_mathtest(void) {
    fixed_t a = float_to_fixed(1.5f);
    fixed_t b = float_to_fixed(2.0f);
    fixed_t c = fixed_mul(a, b);
    fixed_t d = fixed_div(c, a);
    
    syscall_print("Math test (Fixed-point Q16.16):\n");
    syscall_print("  1.5 * 2.0 = ");
    shell_print_uint(fixed_to_int(c));
    syscall_print(".");
    shell_print_uint((c & 0xFFFF) * 1000 / 65536);
    syscall_print("\n");
    
    syscall_print("  3.0 / 1.5 = ");
    shell_print_uint(fixed_to_int(d));
    syscall_print(".");
    shell_print_uint((d & 0xFFFF) * 1000 / 65536);
    syscall_print("\n");

    fixed_t s = fixed_sigmoid(0);
    syscall_print("  sigmoid(0) = ");
    shell_print_uint(fixed_to_int(s));
    syscall_print(".");
    shell_print_uint((s & 0xFFFF) * 1000 / 65536);
    syscall_print(" (expected 0.5)\n");
}

static void shell_load_model(const char *filename) {
    char path[64];

    if (!shell_resolve_path(filename, path, sizeof(path))) {
        syscall_print("Error: Path too long.\n");
        return;
    }

    syscall_print("Loading model: ");
    syscall_print(path);
    syscall_print("...\n");

    if (syscall_llm_load_file(path)) {
        syscall_print("Model loaded successfully into neural engine.\n");
    } else {
        syscall_print("Error: Failed to load supported model from disk.\n");
    }
}

static void shell_heaptest(void) {
    syscall_print("Running kheap selftest...\n");
    if (syscall_heap_test()) {
        syscall_print("heaptest: OK\n");
    } else {
        syscall_print("heaptest: FAILED\n");
    }
}

static char *next_token(char **cursor) {
    char *start = *cursor;

    while (*start == ' ') {
        start++;
    }
    if (!*start) {
        *cursor = start;
        return 0;
    }

    char *end = start;
    while (*end && *end != ' ') {
        end++;
    }
    if (*end) {
        *end = '\0';
        end++;
    }
    *cursor = end;
    return start;
}

static int shell_strncasecmp(const char *s1, const char *s2, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        char c1 = s1[i];
        char c2 = s2[i];
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return (int)(c1 - c2);
        if (c1 == '\0') return 0;
    }
    return 0;
}

static void shell_autocomplete(char *line, int *idx) {
    char *last_space = strrchr(line, ' ');
    char *prefix;
    int is_command = 0;
    
    if (!last_space) {
        prefix = line;
        is_command = 1;
    } else {
        prefix = last_space + 1;
    }
    
    uint32_t plen = strlen(prefix);
    if (plen == 0 && !is_command) return;

    if (is_command) {
        const char *cmds[] = {"help", "clear", "status", "mem", "mem test", "highmemtest", "ls", "cat", "cd", "loadmodel", "llm", 0};
        for (int i = 0; cmds[i]; i++) {
            if (shell_strncasecmp(cmds[i], prefix, plen) == 0) {
                strncpy(line, cmds[i], 63);
                line[63] = '\0';
                *idx = strlen(line);
                return;
            }
        }
    } else {
        // File completion
        char name[64];
        for (int i = 0; i < 64; i++) {
            if (syscall_fat_get_entry(shell_cwd[0] ? shell_cwd : "/", i, name)) {
                if (shell_strncasecmp(name, prefix, plen) == 0) {
                    uint32_t base_idx = (last_space + 1 - line);
                    strncpy(last_space + 1, name, 63 - base_idx);
                    line[63] = '\0';
                    *idx = base_idx + strlen(last_space + 1);
                    return;
                }
            } else break;
        }
    }
}

static void shell_net_config_static(char *args) {
    char *cursor = args;
    char *ip = next_token(&cursor);
    char *mask = next_token(&cursor);
    char *gw = next_token(&cursor);

    if (!ip || !mask || !gw) {
        syscall_print("Usage: net config static <ip> <mask> <gw>\n");
        return;
    }

    if (syscall_net_config_static(ip, mask, gw)) {
        syscall_print("Network static config updated.\n");
    } else {
        syscall_print("Invalid network config.\n");
    }
}

static void shell_handle_command(char *line) {
    if (line[0] == '!') {
        syscall_put_char('\n');
        shell_run_llm_query(line + 1);
    } else if (strcmp(line, "help") == 0) {
        syscall_print("Commands: help, clear, status, mem, mem map, mem test, highmemtest, heaptest, ls [dir], cat <file>, cd <dir>, loadmodel <file>, extls, extcat <file>, fs, gpu, gpu info, net status, net config dhcp, net config static <ip> <mask> <gw>, arp, llm status, llm info, llm selftest, llm trace on|off|status, llm net on|off|status|port <p>, llm ask <p>, !<p>\n");
    } else if (strcmp(line, "ls") == 0) {
        syscall_fat_ls(shell_cwd[0] ? shell_cwd : 0);
    } else if (strstr(line, "ls ") == line) {
        char path[64];
        if (!shell_resolve_path(line + 3, path, sizeof(path))) {
            syscall_print("Error: Path too long.\n");
        } else {
            syscall_fat_ls(path);
        }
    } else if (strstr(line, "cd ") == line) {
        char resolved[64];
        char *path = line + 3;
        if (!shell_resolve_path(path, resolved, sizeof(resolved))) {
            syscall_print("Error: Path too long.\n");
        } else if (!resolved[0] || syscall_fat_is_dir(resolved)) {
            strncpy(shell_cwd, resolved, sizeof(shell_cwd) - 1);
            shell_cwd[sizeof(shell_cwd) - 1] = '\0';
        } else {
            syscall_print("Directory not found.\n");
        }
    } else if (strstr(line, "cat ") == line) {
        char path[64];
        if (!shell_resolve_path(line + 4, path, sizeof(path))) {
            syscall_print("Error: Path too long.\n");
        } else {
            syscall_fat_cat(path);
        }
    } else if (strstr(line, "loadmodel ") == line) {
        shell_load_model(line + 10);
    } else if (strcmp(line, "extls") == 0) {
        syscall_ext_ls();
    } else if (strstr(line, "extcat ") == line) {
        syscall_ext_cat(line + 7);
    } else if (strcmp(line, "fs") == 0) {
        syscall_print("Storage: FAT32 (Mounted), Initrd (Mounted).\n");
    } else if (strcmp(line, "gpu") == 0 || strcmp(line, "gpu info") == 0) {
        shell_gpu_info();
    } else if (strcmp(line, "net status") == 0) {
        char buf[256];
        syscall_net_status_str(buf);
        syscall_print(buf);
    } else if (strcmp(line, "net config dhcp") == 0) {
        if (syscall_net_config_dhcp()) {
            syscall_print("Network mode set to DHCP.\n");
        } else {
            syscall_print("Failed to set DHCP mode.\n");
        }
    } else if (strstr(line, "net config static ") == line) {
        shell_net_config_static(line + 18);
    } else if (strcmp(line, "arp") == 0 || strcmp(line, "net arp") == 0) {
        shell_arp_info();
    } else if (strcmp(line, "llm status") == 0) {
        char buf[256];
        syscall_llm_status_str(buf);
        syscall_print(buf);
    } else if (strcmp(line, "llm info") == 0) {
        char buf[256];
        syscall_llm_info_str(buf);
        syscall_print(buf);
    } else if (strcmp(line, "llm selftest") == 0 || strcmp(line, "llm test") == 0) {
        shell_llm_selftest();
    } else if (strstr(line, "llm trace ") == line) {
        shell_llm_trace_command(line + 10);
    } else if (strstr(line, "llm net ") == line) {
        shell_llm_net_command(line + 8);
    } else if (strstr(line, "llm ask ") == line) {
        shell_run_llm_query(line + 8);
    } else if (strstr(line, "touch ") == line) {
        char path[64];
        if (!shell_resolve_path(line + 6, path, sizeof(path))) {
            syscall_print("Error: Path too long.\n");
        } else if (syscall_fat_create(path)) {
            syscall_print("File created.\n");
        } else {
            syscall_print("Failed to create file.\n");
        }
    } else if (strstr(line, "write ") == line) {
        char *path = line + 6;
        char *data = strchr(path, ' ');
        if (data) {
            char resolved[64];
            *data = '\0';
            data++;
            if (!shell_resolve_path(path, resolved, sizeof(resolved))) {
                syscall_print("Error: Path too long.\n");
            } else {
                syscall_fat_write(resolved, data, strlen(data));
                syscall_print("Write command sent.\n");
            }
        } else {
            syscall_print("Usage: write <file> <data>\n");
        }
    } else if (strstr(line, "rm ") == line) {
        char path[64];
        if (!shell_resolve_path(line + 3, path, sizeof(path))) {
            syscall_print("Error: Path too long.\n");
        } else if (syscall_fat_delete(path)) {
            syscall_print("File deleted.\n");
        } else {
            syscall_print("Failed to delete file.\n");
        }
    } else if (strstr(line, "mkdir ") == line) {
        char path[64];
        if (!shell_resolve_path(line + 6, path, sizeof(path))) {
            syscall_print("Error: Path too long.\n");
        } else if (syscall_fat_mkdir(path)) {
            syscall_print("Directory created.\n");
        } else {
            syscall_print("Failed to create directory.\n");
        }
    } else if (strcmp(line, "status") == 0) {
        syscall_print("MicroK AI-OS Stable. Shell recovered.\n");
    } else if (strcmp(line, "mem") == 0) {
        shell_mem();
    } else if (strcmp(line, "mem map") == 0) {
        shell_mem_map();
    } else if (strcmp(line, "mem test") == 0 || strcmp(line, "highmemtest") == 0) {
        if (syscall_highmem_test()) {
            syscall_print("PAE High Memory Verification: PASSED\n");
        } else {
            syscall_print("PAE High Memory Verification: FAILED\n");
        }
    } else if (strcmp(line, "mathtest") == 0) {
        shell_mathtest();
    } else if (strcmp(line, "heaptest") == 0) {
        shell_heaptest();
    } else if (strcmp(line, "clear") == 0) {
        syscall_clear();
    } else {
        syscall_print("Unknown command: ");
        syscall_print(line);
        syscall_print("\n");
    }
}

void shell_task(void) {
    char line_buf[64];
    int line_idx = 0;
    uint8_t show_prompt = 1;
    
    while (1) {
        syscall_net_poll();

        if (show_prompt) {
            // Build prompt
            syscall_print("MicroK ");
            if (shell_cwd[0]) {
                syscall_print("/");
                syscall_print(shell_cwd);
            }
            syscall_print("> ");
            show_prompt = 0;
        }

        char c = syscall_get_char();

        if (c != 0) {
            if (c == '\n') {
                syscall_put_char('\n');
                line_buf[line_idx] = '\0';

                if (line_idx > 0) {
                    shell_handle_command(line_buf);
                }

                line_idx = 0;
                memset(line_buf, 0, sizeof(line_buf));
                show_prompt = 1;
            } else if (c == '\t') {
                line_buf[line_idx] = '\0';
                shell_autocomplete(line_buf, &line_idx);
                // Reprint line (very basic)
                syscall_print("\rMicroK ");
                if (shell_cwd[0]) {
                    syscall_print("/");
                    syscall_print(shell_cwd);
                }
                syscall_print("> ");
                syscall_print(line_buf);
            } else if (c == '\b') {
                if (line_idx > 0) {
                    line_idx--;
                    syscall_put_char('\b');
                }
            } else if (line_idx < 63) {
                line_buf[line_idx++] = c;
                syscall_put_char(c);
            }
        }

        syscall_yield();
    }
}
