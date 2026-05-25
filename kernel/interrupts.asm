[bits 32]

global idt_load
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

%macro ISR_NOERR 1
global isr%1
isr%1:
    pusha
    mov eax, esp
    push eax
    push 0
    push %1
    call fault_handler
.halt%1:
    cli
    hlt
    jmp .halt%1
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    pusha
    mov eax, [esp + 32] ; CPU-pushed error code after pusha frame
    mov ebx, esp
    push ebx
    push eax
    push %1
    call fault_handler
.halt%1:
    cli
    hlt
    jmp .halt%1
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

global irq0
global isr128
extern isr_handler
extern fault_handler
extern irq_handler
extern syscall_handler

irq0:
    pusha
    push 0      ; IRQ 0
    call irq_handler
    add esp, 4
    popa
    iret

global irq1
irq1:
    pusha
    push 1      ; IRQ 1
    call irq_handler
    add esp, 4
    popa
    iret

isr128:
    pusha
    push ebx ; arg3
    push edx ; arg2
    push ecx ; arg1
    push eax ; syscall_num
    call syscall_handler
    add esp, 16 ; Remove args from stack
    mov [esp + 28], eax ; Update EAX in the pusha frame (EAX is at the top)
    popa
    iret

global load_page_directory
load_page_directory:
    mov eax, [esp + 4]
    mov cr3, eax
    ret

global enable_paging
enable_paging:
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ret
