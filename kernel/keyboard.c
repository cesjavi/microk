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

static void kbd_wait_input_clear() {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 0x02)) return;
    }
}

static void kbd_send_device_byte(uint8_t val) {
    kbd_wait_input_clear();
    outb(0x60, val);
}

static void kbd_wait_ack() {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) {
            inb(0x60); // consume ACK (0xFA) or whatever the device sent; don't hang on a nonstandard reply
            return;
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

    // Slow down hardware key-repeat (0xF3 = Set Typematic Rate/Delay).
    // keyboard_handler() sets last_char on every make-code with no
    // debounce, so it relies on the PS/2 device's own repeat pacing to
    // avoid flooding the shell's line buffer. The controller's power-on
    // default can repeat as fast as ~30Hz after only a 250ms delay -- fine
    // under QEMU where test input was short discrete keystrokes, but a
    // normal human keypress under VirtualBox's PS/2 emulation can register
    // as several repeats before the key is released (e.g. "s" -> "ssss").
    // 0x7F = slowest rate (~2Hz) with the longest delay (1000ms) before
    // repeat kicks in. Polled directly (no IRQs enabled globally yet at
    // this point in boot), same reasoning as the timer-tick traps already
    // documented for UHCI/net init in ROADMAP.md.
    kbd_send_device_byte(0xF3);
    kbd_wait_ack();
    kbd_send_device_byte(0x7F);
    kbd_wait_ack();

    serial_puts("KBD: HW Initialized and Unmasked\n");
}

char keyboard_get_char() {
    char c = last_char;
    last_char = 0;
    return c;
}
