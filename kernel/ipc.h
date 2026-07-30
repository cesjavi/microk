#ifndef IPC_H
#define IPC_H

#include <stdint.h>

#define MAX_PORTS 1024
#define MSG_PAYLOAD_SIZE 256

typedef struct {
    uint32_t sender;
    uint32_t type;
    uint32_t length;
    uint8_t  payload[MSG_PAYLOAD_SIZE];
} message_t;

typedef struct {
    uint32_t id;
    uint32_t owner_task;
    int active;
    int has_message;
    message_t message;
} port_t;

void ipc_init();
int  ipc_send(uint32_t port_id, message_t *msg);
int  ipc_receive(uint32_t port_id, message_t *msg);
uint32_t ipc_create_port(uint32_t owner);

/* Shared memory segments.
 *
 * MicroK currently runs every task (kernel and ring-3 alike) inside a
 * single page directory -- task_switch() only swaps ESP/registers, not
 * CR3 -- so there is no real per-task address space yet to map a segment
 * into selectively. Until "User mode real" gives each task its own
 * virtual memory, this provides the SysV-shm-style allocate/attach/detach
 * API and a stable key->id->address mapping rather than actual cross-
 * address-space mapping; any task can already see any other task's
 * memory, so the safety value here is the validated, refcounted handle,
 * not isolation. */
#define IPC_SHM_MAX_SEGMENTS 16

int ipc_shm_create(uint32_t key, uint32_t size, uint32_t *out_id);
int ipc_shm_lookup_key(uint32_t key, uint32_t *out_id);
int ipc_shm_attach(uint32_t id, uint32_t *out_addr, uint32_t *out_size);
int ipc_shm_detach(uint32_t id);
int ipc_shm_destroy(uint32_t id);
int ipc_shm_selftest(void);

#endif
