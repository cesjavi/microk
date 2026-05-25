# MicroK PAE / High Memory Plan

MicroK currently runs as a 32-bit Multiboot kernel with classic 32-bit paging.
That means the PMM can reason about RAM below 4GB, but the VMM cannot map
physical addresses above 4GB. Full RAM usage on real hardware needs either PAE
in protected mode or a migration to long mode.

## Current State

- Multiboot memory map entries are parsed.
- Usable RAM regions below 4GB are released into the PMM.
- Reserved, ACPI, MMIO, firmware, and invalid/overlapping mmap entries are kept
  visible through `mem` and `mem map`.
- `phys_addr_t` exists as the physical-address abstraction, but most allocator
  paths still return low 32-bit addresses.
- High-memory regions above 4GB are detected from Multiboot and reported by
  `mem` as a separate high pool, but that pool is not allocatable yet.
- Runtime paging still uses 32-bit page directory/page table entries.
- PAE structs and 64-bit entry helpers exist in `kernel/vmm_pae.h` and
  `kernel/vmm_pae.c`, but they are not active in `vmm_init`.
- The kernel uses identity mapping for the current 32-bit address space.

## Constraint

PAE expands physical addresses, not virtual addresses. In 32-bit protected mode
with PAE, MicroK can access physical memory above 4GB only by mapping windows of
high physical pages into the 32-bit virtual address space. It still cannot have a
flat virtual address space larger than 4GB.

## Recommended Path

The pragmatic path is staged PAE, not an immediate long-mode rewrite:

1. Keep the kernel 32-bit.
2. Add 64-bit physical frame descriptors to the PMM.
3. Keep low-memory allocations for boot-critical structures.
4. Add high-memory pages to a separate PMM pool.
5. Introduce temporary high-memory mapping windows.
6. Convert the VMM to PAE page tables.
7. Use high memory first for large buffers: model tensors, file cache, network
   buffers, and storage cache.

Long mode remains a later option if MicroK needs a native 64-bit virtual address
space, a cleaner ABI, or larger direct mappings.

## PMM Changes

Required PMM changes:

- Convert allocator paths from low-only `uint32_t` physical addresses to
  `phys_addr_t`.
- Store high frames separately from the low bitmap when the bootloader reports
  regions above 4GB.
- Preserve a low-memory pool below 4GB for:
  - page tables,
  - DMA buffers required by legacy devices,
  - boot modules,
  - early kernel heap,
  - identity-mapped compatibility code.
- Add allocation flags:
  - `PMM_ALLOC_LOW`: physical address below 4GB.
  - `PMM_ALLOC_HIGH_OK`: high memory allowed.
  - `PMM_ALLOC_DMA32`: below 4GB and aligned for devices.

## VMM Changes

Required VMM changes:

- Replace the current 2-level 32-bit paging layout with PAE paging:
  - PDPT: 4 entries.
  - Page directories: 512 entries each.
  - Page tables: 512 entries each.
  - 64-bit PTE/PDE entries.
- Enable CR4.PAE before enabling paging with PAE tables.
- Keep NX disabled initially unless EFER/NXE handling is added.
- Preserve existing user/supervisor and read/write permission behavior.
- Add explicit MMIO mapping helpers for PCI BARs and framebuffers.

## High-Memory Mapping Model

Initial high-memory access should use temporary mapping windows:

- Reserve one or more kernel virtual ranges as high-memory windows.
- Map a selected high physical page into a window.
- Read/write/copy data.
- Unmap or replace the mapping.

This avoids requiring a large direct-map region in 32-bit mode.

Recommended first users:

- GGUF tensor streaming.
- Model file cache.
- Block cache.
- Network RX/TX buffers only when the NIC supports the physical address range.

## Safety Rules

- Never allocate page tables from high memory until the VMM can address them
  reliably.
- Keep all boot-critical structures below 4GB.
- Treat PCI MMIO holes as reserved even if adjacent regions are usable.
- Do not expose high-memory pages to user mode until syscall pointer validation
  is stronger.
- Keep `mem` and `mem map` diagnostics working before and after PAE.

## Milestones

- [x] Add `phys_addr_t` and allocation flags.
- [x] Track high memory separately in PMM stats.
- [x] Extend `mem` with high-memory totals.
- [x] Expose low/high pool stats.
- [ ] Add real high-pool allocation after PAE mapping exists.
- [x] Implement PAE page table structs and entry helpers.
- [ ] Add a QEMU target with RAM above 4GB for testing.
- [ ] Add temporary high-memory mapping windows.
- [ ] Move large model/cache buffers to high memory.
- [ ] Decide whether long mode is still needed after staged PAE.
