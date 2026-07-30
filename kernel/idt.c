#include "idt.h"
#include "io.h"
#include "task.h"

struct idt_entry idt[256];
struct idt_ptr idp;

extern void idt_load(uint32_t);
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();
extern void irq0();
extern void irq1();
extern void isr128();

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

void idt_init() {
    // PIC Remapping
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    // Only unmask IRQ0 (timer) and IRQ1 (keyboard). Other IRQs have no stubs yet.
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);

    idp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idp.base = (uint32_t)(uintptr_t)&idt;

    for(int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    idt_set_gate(0, (uint32_t)(uintptr_t)isr0, 0x08, 0x8E);
    idt_set_gate(1, (uint32_t)(uintptr_t)isr1, 0x08, 0x8E);
    idt_set_gate(2, (uint32_t)(uintptr_t)isr2, 0x08, 0x8E);
    idt_set_gate(3, (uint32_t)(uintptr_t)isr3, 0x08, 0x8E);
    idt_set_gate(4, (uint32_t)(uintptr_t)isr4, 0x08, 0x8E);
    idt_set_gate(5, (uint32_t)(uintptr_t)isr5, 0x08, 0x8E);
    idt_set_gate(6, (uint32_t)(uintptr_t)isr6, 0x08, 0x8E);
    idt_set_gate(7, (uint32_t)(uintptr_t)isr7, 0x08, 0x8E);
    idt_set_gate(8, (uint32_t)(uintptr_t)isr8, 0x08, 0x8E);
    idt_set_gate(9, (uint32_t)(uintptr_t)isr9, 0x08, 0x8E);
    idt_set_gate(10, (uint32_t)(uintptr_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)(uintptr_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)(uintptr_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)(uintptr_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)(uintptr_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)(uintptr_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)(uintptr_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)(uintptr_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)(uintptr_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)(uintptr_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)(uintptr_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)(uintptr_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)(uintptr_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)(uintptr_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)(uintptr_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)(uintptr_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)(uintptr_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)(uintptr_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)(uintptr_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)(uintptr_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)(uintptr_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)(uintptr_t)isr31, 0x08, 0x8E);
    idt_set_gate(32, (uint32_t)(uintptr_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint32_t)(uintptr_t)irq1, 0x08, 0x8E);
    idt_set_gate(128, (uint32_t)(uintptr_t)isr128, 0x08, 0xEE);

    idt_load((uint32_t)(uintptr_t)&idp);
}

extern void timer_handler();
extern void keyboard_handler();

void irq_handler(int irq) {
    if (irq == 0) {
        outb(0x20, 0x20);
        timer_handler();
        return;
    } else if (irq == 1) {
        keyboard_handler();
    }
    
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void isr_handler(void) {
    char *video_memory = (char *) 0xb8000;
    video_memory[158] = '!';
    video_memory[159] = 0x0C;
}

#include "video.h"
#include "serial.h"

typedef struct fault_regs {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
} fault_regs_t;

static uint32_t read_cr2(void) {
    uint32_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static void print_hex32(const char *label, uint32_t value) {
    char out[] = "0x00000000";
    const char *hex = "0123456789ABCDEF";

    for (int i = 0; i < 8; i++) {
        out[2 + i] = hex[(value >> (28 - (i * 4))) & 0xF];
    }

    kprint(label);
    kprint(out);
    kprint("\n");
}

static const char *fault_name(uint32_t vector) {
    static const char *names[32] = {
        "divide error",
        "debug",
        "non-maskable interrupt",
        "breakpoint",
        "overflow",
        "bound range exceeded",
        "invalid opcode",
        "device not available",
        "double fault",
        "coprocessor segment overrun",
        "invalid TSS",
        "segment not present",
        "stack-segment fault",
        "general protection fault",
        "page fault",
        "reserved",
        "x87 floating-point exception",
        "alignment check",
        "machine check",
        "SIMD floating-point exception",
        "virtualization exception",
        "control protection exception",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "reserved",
        "hypervisor injection exception",
        "VMM communication exception",
        "security exception",
        "reserved"
    };

    if (vector < 32) {
        return names[vector];
    }
    return "unknown exception";
}

static void print_page_fault_bits(uint32_t error_code) {
    klog((error_code & 0x01) ? "PF: protection violation." : "PF: non-present page.");
    klog((error_code & 0x02) ? "PF: write access." : "PF: read access.");
    klog((error_code & 0x04) ? "PF: user mode access." : "PF: supervisor access.");
    if (error_code & 0x08) {
        klog("PF: reserved page-table bit set.");
    }
    if (error_code & 0x10) {
        klog("PF: instruction fetch.");
    }
}

/* Vectors for which the CPU pushes an error code (ISR_ERR in interrupts.asm) */
static int fault_has_error_code(uint32_t vector) {
    return vector == 8  || vector == 10 || vector == 11 || vector == 12 ||
           vector == 13 || vector == 14 || vector == 17 || vector == 21 ||
           vector == 30;
}

void fault_handler(uint32_t vector, uint32_t error_code, fault_regs_t *regs) {
    /*
     * Stack layout above the pusha frame (regs pointer):
     *   ISR_NOERR: regs[8]=EIP  regs[9]=CS  regs[10]=EFLAGS  [regs[11]=ESP_user  regs[12]=SS if ring3]
     *   ISR_ERR:   regs[8]=err  regs[9]=EIP regs[10]=CS      regs[11]=EFLAGS     [regs[12]=ESP_user...]
     */
    uint32_t cs = 0;
    uint32_t eip = 0;
    if (regs) {
        uint32_t *stack = (uint32_t *)regs;
        cs = fault_has_error_code(vector) ? stack[10] : stack[9];
        eip = fault_has_error_code(vector) ? stack[9] : stack[8];
    }
    int from_user = (cs & 3) == 3;

    if (from_user) {
        klog("USER FAULT: exception in ring-3 task — killing task.");
        kprint("Fault: ");
        kprint(fault_name(vector));
        kprint(" (user task killed)\n");
        print_hex32("Vector: ", vector);
        if (error_code) print_hex32("Error code: ", error_code);
        if (vector == 14) {
            print_hex32("CR2: ", read_cr2());
            print_page_fault_bits(error_code);
        }
        task_kill_current();
        /* task_kill_current() switches away; we never reach here */
        while (1) { asm volatile("hlt"); }
    }

    klog("KERNEL FAULT: CPU exception received.");
    kprint("Fault: ");
    kprint(fault_name(vector));
    kprint("\n");

    print_hex32("Vector: ", vector);
    print_hex32("Error code: ", error_code);
    print_hex32("EIP: ", eip);
    print_hex32("CS: ", cs);
    if (vector == 14) {
        print_hex32("CR2 fault address: ", read_cr2());
        print_page_fault_bits(error_code);
    }

    if (regs) {
        print_hex32("EAX: ", regs->eax);
        print_hex32("EBX: ", regs->ebx);
        print_hex32("ECX: ", regs->ecx);
        print_hex32("EDX: ", regs->edx);
        print_hex32("ESI: ", regs->esi);
        print_hex32("EDI: ", regs->edi);
        print_hex32("EBP: ", regs->ebp);
        print_hex32("Saved ESP: ", regs->esp);
    }

    while (1) {
        asm volatile("cli; hlt");
    }
}
