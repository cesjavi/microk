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
    if (task_count >= MAX_TASKS) return;

    task_t *new_task = &task_list[task_count];
    uint32_t *stack = (uint32_t *)pmm_alloc_block();
    if (!stack) return;
    
    uint32_t *stk_ptr = stack + (PAGE_SIZE / sizeof(uint32_t));
    
    // IRET Frame
    *(--stk_ptr) = 0x202;           // EFLAGS (IF=1)
    *(--stk_ptr) = 0x08;            // CS (Kernel Code)
    *(--stk_ptr) = (uint32_t)entry; // EIP (Task Entry Point)
    
    // Switch Context Return
    *(--stk_ptr) = (uint32_t)task_start_stub; // Return address for 'ret' in switch_context
    
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
    
    new_task->esp = (uint32_t)stk_ptr;
    new_task->page_dir = page_directory; // For now sharing kernel page dir
    new_task->state = 1; // Ready
    
    task_count++;
}

extern void switch_context(uint32_t *prev, uint32_t *next);

void task_switch() {
    if (task_count <= 1) return;

    int prev_id = current_task_id;
    current_task_id = (current_task_id + 1) % task_count;
    task_list[prev_id].state = 1;
    task_list[current_task_id].state = 2;
    
    switch_context(&task_list[prev_id].esp, &task_list[current_task_id].esp);
}
