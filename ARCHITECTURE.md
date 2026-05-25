# MicroK Architecture

MicroK is currently a small experimental kernel with an AI-oriented shell and a
minimal in-kernel model runtime. The long-term vision is microkernel-like, but
the current implementation still keeps most drivers and services in kernel
space.

## Current Architecture

- **Boot**: Multiboot v1 kernel loaded by QEMU/GRUB.
- **CPU setup**: GDT, IDT, PIC, PIT.
- **Memory**: PMM bitmap and identity-mapped paging over available low memory.
- **I/O**: VGA text output, serial logging, PS/2 keyboard.
- **Syscalls**: `int 0x80` interface used by the shell path.
- **Shell**: foreground interactive shell for the stable demo.
- **LLM runtime**: MKLM model loader with MKRP rules, MKNN neural intents, and a
  minimal GGUF header probe.
- **Storage**: experimental VFS, block devices, ATA PIO, MBR/GPT, FAT32 and
  filesystem probes.
- **Hardware discovery**: PCI scan and basic GPU vendor detection.

## Stable Demo Flow

1. QEMU boots `build/kernel.bin`.
2. QEMU/GRUB provides `models/model.mklm` as a module/initrd.
3. The kernel detects MKLM magic and registers the model.
4. The shell starts in foreground.
5. `llm ask ...` runs MKNN inference from the loaded model.

## Not Implemented Yet

- User mode isolation.
- Robust preemptive multitasking.
- Higher-half kernel.
- MLFQ scheduler.
- User-space drivers/translators.
- Shared memory IPC.
- Full filesystem navigation for ext4/NTFS.
- GGUF inference.
- GPU compute acceleration.

## Intended Direction

The desired architecture is still microkernel-inspired: drivers and services
should eventually move out of the core kernel, communicating via IPC. Before
that, the project needs stronger exception handling, memory safety, scheduler
stability, and a cleaner split between boot, shell, storage, and LLM modules.
