#include "spinlock.h"

void spin_init(spinlock_t *lock) {
    lock->locked = 0;
}

void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        asm volatile("pause");
    }
}

void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

uint32_t spin_lock_irqsave(spinlock_t *lock) {
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags));
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint32_t flags) {
    spin_unlock(lock);
    asm volatile("pushl %0; popfl" : : "r"(flags));
}
