#include "ipc.h"
#include <string.h>

port_t port_registry[MAX_PORTS];

void ipc_init() {
    for (int i = 0; i < MAX_PORTS; i++) {
        port_registry[i].active = 0;
        port_registry[i].has_message = 0;
        port_registry[i].id = i;
    }
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
