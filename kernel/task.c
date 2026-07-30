#include "task.h"
#include "pmm.h"

task_t task_list[MAX_TASKS];
int current_task_id = 0;
int task_count = 0;

extern uint32_t *page_directory; // Kernel's page directory

void task_init() {
    // Current state (kernel) is task 0
    task_list[0].state = 2; // Running
    task_list[0].page_dir = page_directory;
    task_count = 1;
}

extern void task_start_stub();

void task_create(void (*entry)()) {
    int slot = -1;
    for (int i = 1; i < task_count; i++) {
        if (task_list[i].state == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (task_count >= MAX_TASKS) return;
        slot = task_count++;
    }

    task_t *new_task = &task_list[slot];
    uint32_t *stack = (uint32_t *)pmm_alloc_block();
    if (!stack) return;

    uint32_t *stk_ptr = stack + (PAGE_SIZE / sizeof(uint32_t));
    
    // IRET Frame
    *(--stk_ptr) = 0x202;           // EFLAGS (IF=1)
    *(--stk_ptr) = 0x08;            // CS (Kernel Code)
    *(--stk_ptr) = (uint32_t)(uintptr_t)entry; // EIP (Task Entry Point)
    
    // Switch Context Return
    *(--stk_ptr) = (uint32_t)(uintptr_t)task_start_stub; // Return address for 'ret' in switch_context
    
    // PUSHA + EBP
    *(--stk_ptr) = 0;              // EBP (for 'pop ebp')
    *(--stk_ptr) = 0;              // EAX
    *(--stk_ptr) = 0;              // ECX
    *(--stk_ptr) = 0;              // EDX
    *(--stk_ptr) = 0;              // EBX
    *(--stk_ptr) = 0;              // ESP
    *(--stk_ptr) = 0;              // EBP
    *(--stk_ptr) = 0;              // ESI
    *(--stk_ptr) = 0;              // EDI
    
    new_task->esp = (uint32_t)(uintptr_t)stk_ptr;
    new_task->page_dir = page_directory; // For now sharing kernel page dir
    new_task->stack_base = stack;
    new_task->state = 1; // Ready
}

extern void switch_context(uint32_t *prev, uint32_t *next);

void task_switch() {
    if (task_count <= 1) return;

    int prev_id = current_task_id;
    int next_id = (current_task_id + 1) % task_count;

    // Skip dead tasks (state == 0) to avoid restoring a stale/freed stack
    int attempts = 0;
    while (task_list[next_id].state == 0 && attempts < task_count) {
        next_id = (next_id + 1) % task_count;
        attempts++;
    }
    if (task_list[next_id].state == 0) return; // all other tasks are dead

    current_task_id = next_id;
    task_list[prev_id].state = 1;
    task_list[current_task_id].state = 2;

    switch_context(&task_list[prev_id].esp, &task_list[current_task_id].esp);
}

void task_kill_current() {
    if (task_count <= 1) {
        asm volatile("cli; hlt");
        return;
    }

    int dead_id = current_task_id;
    int next_id = (dead_id + 1) % task_count;

    int attempts = 0;
    while (task_list[next_id].state == 0 && attempts < task_count) {
        next_id = (next_id + 1) % task_count;
        attempts++;
    }
    if (task_list[next_id].state == 0) {
        asm volatile("cli; hlt");
        return;
    }

    current_task_id = next_id;
    task_list[dead_id].state = 0;
    task_list[next_id].state = 2;

    if (task_list[dead_id].stack_base) {
        pmm_free_block(task_list[dead_id].stack_base);
        task_list[dead_id].stack_base = 0;
    }

    switch_context(&task_list[dead_id].esp, &task_list[next_id].esp);
}
