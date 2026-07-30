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

; The kernel is built with -msse2 -mfpmath=sse, so ordinary "integer-looking"
; C code (memcpy/memset, struct copies, and of course any float/fixed_t math)
; can legally be compiled down to XMM instructions. pusha/popa only cover the
; eight general-purpose registers -- they never touched XMM/x87 state. IRQ0
; (timer) fires ~100 times/sec regardless of whether it preempts, and IRQ0's
; timer_handler() calls task_switch() every 10 ticks, so any in-flight XMM
; register holding a live float (e.g. mid-dot-product in the LLM matmul) got
; silently clobbered by whatever the interrupted handler or the next
; scheduled task did with XMM in between. This surfaced as h_sumsq
; intermittently overflowing mid-forward-pass once a model was slow enough
; to take multiple timer ticks per layer (TinyLlama's 22 layers vs.
; stories15M's 6 -- small models finished fast enough to rarely straddle a
; tick). FXSAVE/FXRSTOR around the C handler call fixes this: the saved area
; is pushed onto the CURRENT task's own stack, so it survives correctly
; across a task_switch() round trip (each task restores from its own stack).
; Uses ESI as scratch (not EAX/ECX/EDX/EBX) so it's safe to splice in between
; pusha and a cdecl call that still needs EAX/ECX/EDX/EBX live as arguments
; (isr128 passes the syscall number/args in those registers). ESI's original
; value is already safe in the pusha frame on the stack; popa restores it
; from there regardless of what we do to the live register here.
%macro SAVE_FPU 0
    sub esp, 528          ; 512-byte fxsave area + slack for 16-byte alignment
    mov esi, esp
    add esi, 15
    and esi, 0xFFFFFFF0
    fxsave [esi]
    push esi              ; remember the aligned pointer for the matching restore
%endmacro

%macro RESTORE_FPU 0
    pop esi
    fxrstor [esi]
    add esp, 528
%endmacro

irq0:
    pusha
    SAVE_FPU
    push 0      ; IRQ 0
    call irq_handler
    add esp, 4
    RESTORE_FPU
    popa
    iret

global irq1
irq1:
    pusha
    SAVE_FPU
    push 1      ; IRQ 1
    call irq_handler
    add esp, 4
    RESTORE_FPU
    popa
    iret

isr128:
    pusha
    SAVE_FPU
    push ebx ; arg3
    push edx ; arg2
    push ecx ; arg1
    push eax ; syscall_num
    call syscall_handler
    add esp, 16 ; Remove args from stack
    RESTORE_FPU ; uses ESI as scratch, EAX (syscall return value) survives untouched
    mov [esp + 28], eax ; Update EAX in the pusha frame (EAX is at the top)
    popa
    iret

global load_page_directory
load_page_directory:
    mov eax, [esp + 4]
    mov cr3, eax
    ret

global enable_pae_paging
enable_pae_paging:
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax
    mov eax, [esp + 4]
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ret

global enable_paging
enable_paging:
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ret
