#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "serial.h"

unsigned char kbdus[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0
};

unsigned char kbdus_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0
};

char last_char = 0;
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int abort_requested = 0;

int sys_abort_requested() {
    int val = abort_requested;
    abort_requested = 0;
    return val;
}

void keyboard_handler() {
    uint8_t status = inb(0x64);
    if (status & 0x01) { // Data buffer full
        uint8_t scancode = inb(0x60);

        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = 1;
            return;
        }
        if (scancode == 0xAA || scancode == 0xB6) {
            shift_pressed = 0;
            return;
        }
        if (scancode == 0x1D) {
            ctrl_pressed = 1;
            return;
        }
        if (scancode == 0x9D) {
            ctrl_pressed = 0;
            return;
        }
        
        if (ctrl_pressed && scancode == 0x2E) { // Ctrl + C
            abort_requested = 1;
            serial_puts("\n[!] KERNEL: Abort requested by user (Ctrl+C)\n");
            return;
        }
        
        if (!(scancode & 0x80) && scancode < sizeof(kbdus)) {
            last_char = shift_pressed ? kbdus_shift[scancode] : kbdus[scancode];
        }
    }
}

void keyboard_init() {
    // Disable devices
    outb(0x64, 0xAD);
    outb(0x64, 0xA7);

    // Flush buffer
    while (inb(0x64) & 0x01) inb(0x60);

    // Enable keyboard
    outb(0x64, 0xAE);
    
    // Enable interrupts in controller
    outb(0x64, 0x20); // Command: read configuration
    uint8_t config = inb(0x60);
    config |= 0x01;   // Bit 0: enable interrupt
    outb(0x64, 0x60); // Command: write configuration
    outb(0x60, config);

    serial_puts("KBD: HW Initialized and Unmasked\n");
}

char keyboard_get_char() {
    char c = last_char;
    last_char = 0;
    return c;
}
