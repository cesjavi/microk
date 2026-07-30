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
  `mem` as a separate high pool. The allocator tracks each usable high-memory
  mmap segment separately instead of assuming one contiguous span above 4GB.
- Runtime paging uses PAE when CPUID reports support; otherwise it falls back
  to classic 32-bit paging.
- PAE structs and 64-bit entry helpers exist in `kernel/vmm_pae.h` and
  `kernel/vmm_pae.c`, and `vmm_init` now builds PAE PDPT/PD/PT entries for the
  low identity map.
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
- Preserve high-memory holes by mapping high-pool bitmap indexes back to the
  specific usable mmap segment that owns them.
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
- [x] Make the high pool region-aware instead of assuming contiguous RAM above 4GB.
- [x] Add real high-pool allocation after PAE mapping exists.
- [x] Implement PAE page table structs and entry helpers.
- [x] Activate PAE paging in `vmm_init` when supported by CPUID.
- [x] Add a QEMU target with RAM above 4GB for testing (`make qemu-highmem`).
- [x] Add temporary high-memory mapping windows (`vmm_temp_map_high`).
- [x] Generalize to multiple concurrent windows (`vmm_temp_map_high_slot`, 4 slots) and a general-purpose paged buffer abstraction (`hbuf_t` in `kernel/highmem.c`) that allocates/reads/writes a logically contiguous byte range backed by non-contiguous high-pool pages. Verified in QEMU (`-m 6G`): `highmemtest buf` (syscall 60) passes multi-page alloc + cross-page read/write + out-of-range rejection.
- [x] Move large model/cache buffers (GGUF tensors) to high memory using `hbuf_t` -- **reads only; freeing the original low-memory copy is deliberately disabled, see "Known issue" below.** `gguf_migrate_data_to_high_mem`/`gguf_release_high_mem_backing`/`gguf_read_data_bytes` in `kernel/llm_gguf.c` migrate `[data_offset, file_size)` of a loaded GGUF model into an `hbuf_t` when a high pool is available and the section is at least 8MB; every tensor-data read site (`llama_matmul_fpu`, the vocab argmax scans, `tensor_load_gguf`, `tensor_read_gguf_row_into`) goes through this accessor instead of a raw pointer, using persistent per-file scratch buffers (not malloc/free per row -- an earlier version without this made `llm selftest gguf` ~10x slower). Falls back to the original direct-pointer behavior byte-for-byte when no high memory exists (the common case, verified identical output).
- [ ] Decide whether long mode is still needed after staged PAE.

## Known issue: freeing the original low-memory copy after migration corrupts data (root cause not found)

The original design also freed `[data_offset, file_size)` back to the low pool via
`pmm_free_region()` right after copying to the `hbuf_t`, to actually reclaim the RAM (the
whole point of this milestone). Verification via checksums of 32 sampled rows of
`output.weight` (TinyLlama, Q6_K, `-m 6G`) found **one row out of 32** with a different,
but internally reproducible, checksum between the low-memory and high-memory copies. All
of the following were ruled out by direct testing:

- Copy chunk size (tried 64KB and 4KB -- same exact row fails either way).
- Timer interrupts (still fails with `cli`/`sti` around the whole copy loop).
- Duplicate physical page allocation within the same `hbuf_t` (scanned the full `pages[]`
  array for the affected logical page -- no duplicate).
- The read/write mechanism itself (writing a known test pattern to the exact same
  physical address round-trips correctly).
- Source instability (checksum of the source bytes is identical before and after the
  entire copy loop).
- Flaky reads (re-reading the bad value gives the same wrong value every time, not
  different garbage).

Direct instrumentation inside `hbuf_copy` (`kernel/highmem.c`), logging every touch of the
affected page along with the caller's return address, confirmed that `vmm_temp_map_high_slot`
(called *only* from `hbuf_copy` -- the sole mechanism in this entire kernel that can reach
physical memory above 4GB) touches that page exactly 3 times: the original write (verified
correct immediately, in the same loop iteration), and two of our own later diagnostic
reads. Nothing else. A GDB hardware watchpoint on the exact physical address was considered
and rejected as an approach: it would hit the same wall as a conditional breakpoint, since
all ~148,000 legitimate high-memory writes share the one instrumented code path, and each
hit still needs a remote-protocol condition check. QEMU's monitor `info mtree` confirmed
RAM above 4GB is an internal QEMU alias over the same backing store as low RAM at a
different host offset -- normal QEMU behavior, doesn't look like the cause.

**No root cause found.** Possibly a QEMU/TCG emulation bug specific to >4GB physical
addressing under PAE at this access volume; would need QEMU-internal execution/memory
tracing to pin down further, beyond normal kernel or GDB debugging.

**Mitigation applied**: `gguf_migrate_data_to_high_mem` no longer calls `pmm_free_region()`
on the original low-memory bytes. Reads from the `hbuf_t` are verified correct (energy
traces byte-identical across all 22 layers, 31/32 sampled `output.weight` rows exact
match); the original low-memory copy is just left reserved and unused instead of being
returned to the pool. Safe, but doesn't free the low memory this milestone was meant to
free.

## Build Note: `-mstackrealign`

Discovered while first-time boot-testing this plan's code in QEMU (this build had never
been compiled+booted before — no toolchain existed on this machine until now): GCC's
`-O3` auto-vectorizer emits aligned SSE stores (`movdqa`) for local-array-filling loops,
which assumes the SysV i386 ABI guarantee that the stack is 16-byte aligned on function
entry. MicroK's hand-written interrupt/syscall entry stubs (`kernel/interrupts.asm`)
don't provide that guarantee, so any C function several frames deep in a syscall handler
with such a loop can get a misaligned `movdqa` target and fault with a general protection
fault (vector 0x0D) -- this hit both the new `hbuf_selftest()` and, separately, the
existing `e1000_init_rings()` network path. Fixed globally by adding `-mstackrealign` to
`CFLAGS`, which makes GCC realign the stack in the prologue of any function that needs it
instead of trusting the caller. This is the same class of bug (SSE + interrupt context)
as the FXSAVE/FXRSTOR fix in Fase 1 of ROADMAP.md, but at the compiler level instead of
context-save level.
