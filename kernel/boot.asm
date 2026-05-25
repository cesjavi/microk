; Multiboot2 Header for MicroK
; Inspired by the Multiboot2 specification

section .multiboot
align 4
    dd 0x1BADB002               ; Magic number
    dd 0x00000007               ; Flags (ALIGNED + MEMINFO + GRAPHICS)
    dd -(0x1BADB002 + 0x00000007) ; Checksum
    
    ; Graphics fields (mode_type, width, height, depth)
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
