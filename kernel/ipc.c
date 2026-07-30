#include "ipc.h"
#include "pmm.h"
#include <string.h>

port_t port_registry[MAX_PORTS];

typedef struct {
    uint32_t key;
    uint32_t size;
    uint32_t addr;
    uint32_t ref_count;
    int active;
} ipc_shm_segment_t;

static ipc_shm_segment_t shm_segments[IPC_SHM_MAX_SEGMENTS];

void ipc_init() {
    for (int i = 0; i < MAX_PORTS; i++) {
        port_registry[i].active = 0;
        port_registry[i].has_message = 0;
        port_registry[i].id = i;
    }
    for (int i = 0; i < IPC_SHM_MAX_SEGMENTS; i++) {
        shm_segments[i].active = 0;
        shm_segments[i].ref_count = 0;
    }
}

int ipc_shm_lookup_key(uint32_t key, uint32_t *out_id) {
    for (int i = 0; i < IPC_SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].active && shm_segments[i].key == key) {
            if (out_id) *out_id = (uint32_t)i;
            return 1;
        }
    }
    return 0;
}

int ipc_shm_create(uint32_t key, uint32_t size, uint32_t *out_id) {
    uint32_t existing_id;

    if (size == 0) return 0;
    if (ipc_shm_lookup_key(key, &existing_id)) {
        if (out_id) *out_id = existing_id;
        return 1; /* SysV shmget semantics: existing key returns the same segment */
    }

    for (int i = 0; i < IPC_SHM_MAX_SEGMENTS; i++) {
        if (!shm_segments[i].active) {
            void *addr = pmm_alloc_region(size);
            if (!addr) return 0;
            shm_segments[i].key = key;
            shm_segments[i].size = size;
            shm_segments[i].addr = (uint32_t)addr;
            shm_segments[i].ref_count = 0;
            shm_segments[i].active = 1;
            if (out_id) *out_id = (uint32_t)i;
            return 1;
        }
    }
    return 0;
}

int ipc_shm_attach(uint32_t id, uint32_t *out_addr, uint32_t *out_size) {
    if (id >= IPC_SHM_MAX_SEGMENTS || !shm_segments[id].active) return 0;
    shm_segments[id].ref_count++;
    if (out_addr) *out_addr = shm_segments[id].addr;
    if (out_size) *out_size = shm_segments[id].size;
    return 1;
}

int ipc_shm_detach(uint32_t id) {
    if (id >= IPC_SHM_MAX_SEGMENTS || !shm_segments[id].active) return 0;
    if (shm_segments[id].ref_count > 0) shm_segments[id].ref_count--;
    return 1;
}

int ipc_shm_destroy(uint32_t id) {
    if (id >= IPC_SHM_MAX_SEGMENTS || !shm_segments[id].active) return 0;
    if (shm_segments[id].ref_count > 0) return 0; /* still attached elsewhere */
    pmm_free_region(shm_segments[id].addr, shm_segments[id].size);
    shm_segments[id].active = 0;
    shm_segments[id].ref_count = 0;
    return 1;
}

/* Creates a segment, attaches to it twice (once "by id" and once by
 * re-resolving the key as a second client would), writes a pattern through
 * one handle and reads it back through the other, then tears everything
 * down. Validates the key->id->address bookkeeping end to end. */
int ipc_shm_selftest(void) {
    uint32_t id = 0, id2 = 0, addr = 0, addr2 = 0, size_out = 0;
    const char *pattern = "MICROK_SHM_OK";
    char readback[32];
    int ok;

    if (!ipc_shm_create(0xC0FFEEu, 4096, &id)) return 0;
    if (!ipc_shm_attach(id, &addr, &size_out) || size_out != 4096) return 0;

    memset((void *)addr, 0, sizeof(readback));
    memcpy((void *)addr, pattern, strlen(pattern) + 1);

    if (!ipc_shm_lookup_key(0xC0FFEEu, &id2) || id2 != id) return 0;
    if (!ipc_shm_attach(id2, &addr2, NULL) || addr2 != addr) return 0;

    memset(readback, 0, sizeof(readback));
    memcpy(readback, (void *)addr2, strlen(pattern) + 1);
    ok = (strcmp(readback, pattern) == 0);

    ipc_shm_detach(id);
    ipc_shm_detach(id2);
    ipc_shm_destroy(id);

    return ok ? 1 : 0;
}

// Basic synchronous IPC (placeholder until we have a scheduler)
int ipc_send(uint32_t port_id, message_t *msg) {
    if (!msg || port_id >= MAX_PORTS || !port_registry[port_id].active) {
        return -1;
    }
    if (port_registry[port_id].has_message) {
        return -1;
    }
    
    if (msg->length > MSG_PAYLOAD_SIZE) {
        msg->length = MSG_PAYLOAD_SIZE;
    }
    memcpy(&port_registry[port_id].message, msg, sizeof(message_t));
    port_registry[port_id].has_message = 1;
    return 0;
}

int ipc_receive(uint32_t port_id, message_t *msg) {
    if (!msg || port_id >= MAX_PORTS || !port_registry[port_id].active) {
        return -1;
    }
    if (!port_registry[port_id].has_message) {
        return -1;
    }
    
    memcpy(msg, &port_registry[port_id].message, sizeof(message_t));
    port_registry[port_id].has_message = 0;
    return 0;
}

uint32_t ipc_create_port(uint32_t owner) {
    for (int i = 0; i < MAX_PORTS; i++) {
        if (!port_registry[i].active) {
            port_registry[i].active = 1;
            port_registry[i].has_message = 0;
            port_registry[i].owner_task = owner;
            return i;
        }
    }
    return 0xFFFFFFFF;
}
