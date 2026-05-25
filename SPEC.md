# MicroK Technical Specification

## 1. Introduction
MicroK is an AI-native, microkernel-based operating system. Unlike traditional OSes, MicroK is designed to host and be optimized by artificial intelligence at its core.

### LLM-First Architecture
- **LLM Engine Server**: A central system server that manages model weights and inference contexts.
- **Prompt Syscalls**: Standardized interface for sending queries to the LLM engine.
- **Weight Mapping**: Specialized memory management for fast loading and sharing of model parameters (Zero-Copy).
- **Context Management**: The kernel understands "inference contexts" as a schedulable resource.

## 2. Microkernel API (Syscalls)
The microkernel provides a minimal set of primitives:
- `ipc_send(port, msg)`: Send a message to a specific port.
- `ipc_receive(port)`: Wait for and receive a message.
- `vm_map(addr, size, flags)`: Map a virtual memory region.
- `task_create()`: Spawn a new isolated task.
- `thread_yield()`: Relinquish the CPU.

## 3. Inter-Process Communication (IPC)
IPC is the primary mechanism for system interaction.
- **Synchronous**: The sender blocks until the receiver accepts the message.
- **Asynchronous**: Messages are queued in the kernel (to be implemented in Phase 2).
- **Payload**: Supports inline data (up to 256 bytes) and out-of-line data (using memory mapping).

## 4. Capability System
MicroK uses a capability-based security model.
- Access to resources (ports, tasks, memory) is mediated by "capabilities".
- Capabilities can be passed between tasks via IPC.

## 5. Boot Protocol
MicroK follows the **Multiboot2** specification to remain compatible with standard bootloaders like GRUB.
- Initial state: Protected Mode (32-bit) with paging disabled.
- Kernel responsibility: Transition to Long Mode (64-bit) and initialize identity mapping.
