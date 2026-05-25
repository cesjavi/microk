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

#endif
