#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

void syscall_init();
uint32_t syscall_handler(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, uint32_t arg3);

#endif
