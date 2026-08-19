; Multiboot2 Header for MicroK
; Inspired by the Multiboot2 specification

section .multiboot
align 4
; MICROK_NO_VIDEO_MODE (nasm -D flag): drop the GRAPHICS bit so the
; bootloader never negotiates a video mode itself -- kept as an escape
; hatch for anything still relying on plain VGA text mode. The
; "unsupported graphical mode type <garbage>" abort this used to work
; around was NOT a GRUB bug (see below): the normal build keeps
; requesting 800x600x32 now that the real cause is fixed. Either way
; kernel/video.c already falls back to VGA text mode when mbi->flags bit
; 11 (FRAMEBUFFER_INFO) is unset.
%ifdef MICROK_NO_VIDEO_MODE
%define MB_FLAGS 0x00000003
%else
%define MB_FLAGS 0x00000007
%endif
    dd 0x1BADB002               ; Magic number
    dd MB_FLAGS                 ; Flags (ALIGNED + MEMINFO [+ GRAPHICS])
    dd -(0x1BADB002 + MB_FLAGS) ; Checksum

    ; a.out kludge fields (header_addr/load_addr/load_end_addr/bss_end_addr/
    ; entry_addr) -- unused (MULTIBOOT_AOUT_KLUDGE, bit 16, is never set
    ; above), but GRUB's struct multiboot_header (grub-core, e.g.
    ; include/multiboot.h) is a FIXED C struct that always reserves these
    ; 5 dwords right after the checksum, whether or not the a.out-kludge
    ; bit is set -- the Multiboot v1 spec only says their VALUES are
    ; unused, not that the bytes are absent from the layout. Omitting
    ; them (as this header used to) shifts every field GRUB reads after
    ; checksum by 20 bytes: grub_multiboot_load() (grub-core/loader/i386/
    ; multiboot_mbi.c) ends up reading OUR width/height/depth/next-section
    ; bytes into its mode_type/width/height/depth, so mode_type comes out
    ; as garbage almost certainly outside its known 0/1 cases -- exactly
    ; the "unsupported graphical mode type <garbage>" abort this comment
    ; used to blame on GRUB. Confirmed against GRUB 2.12's actual source
    ; (the version installed while diagnosing this on real UEFI hardware).
    dd 0                        ; header_addr (unused)
    dd 0                        ; load_addr (unused)
    dd 0                        ; load_end_addr (unused)
    dd 0                        ; bss_end_addr (unused)
    dd 0                        ; entry_addr (unused)

    ; Graphics fields (mode_type, width, height, depth) - only consulted by
    ; the bootloader when the GRAPHICS bit above is set.
    dd 0                        ; mode_type: 0 = linear graphics mode
    dd 800                      ; width
    dd 600                      ; height
    dd 32                       ; depth


section .text
extern kernel_main
global _start

_start:
    cli                         ; Disable interrupts
    mov esp, stack_top          ; Setup stack

    ; Enable SSE properly before any C code (compiled with -msse2) or the
    ; FXSAVE/FXRSTOR pair in interrupts.asm's IRQ/syscall handlers runs.
    ; CR0.EM=0/MP=1 is what actually gates execution of SSE arithmetic
    ; instructions (ordinary float math already worked without this, so
    ; QEMU TCG was likely just not enforcing it) -- but CR4.OSFXSR is what
    ; specifically governs whether FXSAVE/FXRSTOR are even valid opcodes
    ; rather than #UD, which is a separate, harder requirement worth
    ; setting explicitly rather than relying on emulator leniency.
    mov eax, cr0
    and eax, 0xFFFFFFFB ; clear EM (bit 2)
    or  eax, 0x00000002 ; set MP (bit 1)
    mov cr0, eax
    mov eax, cr4
    or  eax, 0x00000600 ; set OSFXSR (bit 9) + OSXMMEXCPT (bit 10)
    mov cr4, eax

    push ebx                    ; Pass multiboot info structure
    push eax                    ; Pass multiboot magic
    call kernel_main            ; Jump to C code

    cli
.hang:
    hlt
    jmp .hang

global switch_context
switch_context:
    ; switch_context(uint32_t *old_esp, uint32_t *new_esp)
    push ebp
    mov ebp, esp
    
    pusha
    
    mov eax, [ebp + 8]    ; old_esp
    mov [eax], esp        ; Save current stack pointer
    
    mov eax, [ebp + 12]   ; new_esp
    mov esp, [eax]        ; Load new stack pointer
    
    popa
    pop ebp
    ret

global task_start_stub
task_start_stub:
    ; The stack here contains EIP, CS, EFLAGS
    iret

global gdt_flush
gdt_flush:
    mov eax, [esp + 4]  ; Pointer to gdt_ptr
    lgdt [eax]          ; Load GDTR
    jmp dword 0x08:.reload_cs ; Explicit 32-bit far jump
.reload_cs:
    mov ax, 0x10        ; Kernel Data Selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

global tss_flush
tss_flush:
    mov ax, 0x28      ; Index 5 << 3 = 40 = 0x28
    ltr ax
    ret

global jump_usermode
jump_usermode:
    ; jump_usermode(uint32_t eip, uint32_t esp)
    cli
    mov ebx, [esp + 4] ; User EIP
    mov ecx, [esp + 8] ; User ESP

    mov ax, 0x23      ; User Data Selector (0x20 | 0x03)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x23          ; SS
    push ecx           ; ESP
    pushf              ; EFLAGS
    pop eax
    or eax, 0x200
    push eax
    push 0x1B          ; CS (0x18 | 0x03)
    push ebx           ; EIP
    iret

section .bss
align 16
stack_bottom:
    resb 65536 ; 64 KB stack
global stack_top
stack_top:
