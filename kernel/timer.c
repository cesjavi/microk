#include "timer.h"
#include "io.h"
#include "idt.h"
#include "task.h"

#include "ai_hooks.h"

extern struct kernel_metrics metrics;
uint32_t tick = 0;

void timer_handler() {
    tick++;
    metrics.total_cycles += 10000; // Mock increment
}

void timer_init(uint32_t frequency) {
    // The value we send to the PIT is the value to divide it's input clock
    // (1193180 Hz) by, to get our required frequency and then program it.
    uint32_t divisor = 1193180 / frequency;

    // Send the command byte.
    outb(0x43, 0x36);

    // Divisor has to be sent byte-wise, so split here into upper/lower bytes.
    uint8_t l = (uint8_t)(divisor & 0xFF);
    uint8_t h = (uint8_t)( (divisor>>8) & 0xFF );

    // Send the frequency divisor.
    outb(0x40, l);
    outb(0x40, h);
}

uint32_t timer_get_ticks() {
    return tick;
}
