#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define MAX_TASKS 64

typedef struct {
    uint32_t esp;       // Stack pointer
    uint32_t ebp;       // Base pointer
    uint32_t eip;       // Instruction pointer
    uint32_t *page_dir; // Task's page directory
    uint32_t *stack_base; // Base of the pmm-allocated stack page (0 for the boot task)
    int      state;     // 0=Dead, 1=Ready, 2=Running
} task_t;

void task_init();
void task_create(void (*entry)());
void task_switch();
void task_kill_current();

#endif
