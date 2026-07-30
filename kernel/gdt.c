#include "gdt.h"
#include <string.h>

struct gdt_entry gdt[6] __attribute__((aligned(16)));
struct gdt_ptr gp __attribute__((aligned(16)));
struct tss_entry_struct tss_entry __attribute__((aligned(16)));

extern void gdt_flush(uint32_t);
extern void tss_flush();
extern void serial_puts(const char*);

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;

    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

static void write_tss(int num, uint16_t ss0, uint32_t esp0) {
    uint32_t base = (uint32_t)(uintptr_t)&tss_entry;
    uint32_t limit = sizeof(tss_entry) - 1;

    gdt_set_gate(num, base, limit, 0x89, 0x00);
    
    // Manual clear to avoid dependency on memset
    uint8_t *p = (uint8_t *)&tss_entry;
    for (uint32_t i = 0; i < sizeof(tss_entry); i++) p[i] = 0;

    tss_entry.ss0  = ss0;
    tss_entry.esp0 = esp0;
    tss_entry.iomap_base = sizeof(tss_entry);
}

void tss_set_stack(uint32_t stack) {
    tss_entry.esp0 = stack;
}

__attribute__((target("no-sse")))
void gdt_init() {
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base  = (uint32_t)(uintptr_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                // Null segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment (Kernel)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment (Kernel)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // Code segment (User)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // Data segment (User)
    
    write_tss(5, 0x10, 0); // TSS Entry

    gdt_flush((uint32_t)(uintptr_t)&gp);
    tss_flush();
}
