ENTRY(_start)

SECTIONS {
    . = 1M;

    .text : {
        KEEP(*(.multiboot))
        user_start = .;
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
        user_end = .;
    }

    kernel_end = .;
}
