# MicroK

MicroK is an experimental AI-native operating system kernel. It boots with
Multiboot, exposes a small interactive shell, loads model modules, and can run a
tiny in-kernel neural intent backend called **MKNN** through the **MKLM v1**
model format.

This is not a production OS. It is a research playground for low-level model
loading, kernel services, storage experiments, and bare-metal inference ideas.

## Current Demo

The most stable demo path is:

- boot the kernel in QEMU,
- load `models/model.mklm` as an initrd/module,
- use the shell commands:
  - `llm status`
  - `llm info`
  - `llm trace on`
  - `llm selftest`
  - `mem`
  - `mem map`
  - `heaptest`
  - `gpu info`
  - `net status`
  - `net config static 10.0.2.15 255.255.255.0 10.0.2.2`
  - `llm net on`
  - `llm net status`
  - `ls`
  - `ls models`
  - `loadmodel model.mklm`
  - `loadmodel models/model.mklm`
  - `llm ask hola`
  - `llm ask que estado tiene el kernel`

There is also a reproducible GGUF smoke path:

- `make qemu-gguf`
- `loadmodel tin1.gguf`
  - `llm status`
  - `llm info`
  - `llm trace on`
  - `llm ask hola`

Expected behavior: the model loads and reports either `PARSER-ONLY` or
`GENERATIVE-PREVIEW`, depending on whether the minimal first-token GGUF path is
available for that model.

## Features

- Multiboot v1 boot.
- GDT/IDT/PIC setup.
- PIT timer and PS/2 keyboard.
- PMM/VMM basics.
- PMM uses the Multiboot memory map when available, freeing usable RAM regions below 4GB and reporting separate low/high memory pools.
- `mem` and `mem map` commands with addressable/usable RAM split, high-memory reporting, PMM, boot/module reservation, Multiboot region validation, and heap stats.
- Staged PAE groundwork: `phys_addr_t`, high-memory pool stats, and inactive PAE table helpers.
- Simple kernel heap with pointer validation, free-block coalescing, and `heaptest`.
- VGA text shell.
- Basic `int 0x80` syscalls.
- VFS and experimental storage stack.
- ATA PIO, MBR/GPT, FAT32/ext/NTFS probes.
- Network configuration scaffold with DHCP/static modes, e1000 PCI detection, experimental polling RX/TX rings, `net status`, and a UDP LLM service path.
- FAT32 read-only supports 8.3 names, basic ASCII VFAT long names, and subdirectories.
- `loadmodel` is handled in kernel space and reads FAT32 models into PMM-allocated contiguous memory instead of a fixed shell-adjacent address.
- `loadmodel` reports required KiB, total free low memory, largest contiguous low-memory block, and high-memory pool status before allocating a model buffer.
- Initial ext2 read-only support can list root, read indirect-block files with simple paths, and load MKLM models.
- PCI scan and GPU diagnostics with vendor/device, bus/slot/function, IRQ,
  command/status, and BAR type/address/size reporting.
- NVIDIA PCI diagnostics are present; native NVIDIA acceleration/compute is not.
- MKLM v1 model loader.
- MKNN neural intent backend.
- `llm info` reports parsed model metadata.
- GGUF parser with metadata, tensor lookup, tokenizer metadata access, and
  basic architecture reporting.

## Hardware Support

This section describes the current code-level hardware status, not the long-term
roadmap.

### Working now

- `x86 32-bit` boot via `Multiboot v1`
- `BIOS/GRUB/QEMU` style boot flow
- `PIT` timer
- `PS/2` keyboard
- `serial` console
- `PC speaker`
- `VGA text mode`
- Physical memory management below `4 GB`
- `ATA PIO` storage on the primary master path
- `MBR/GPT` partition detection
- `FAT32` read-oriented access with subdirectories and ASCII VFAT long names
- Initial `ext2` read-only path
- `PCI` bus scan and device enumeration

### Detected / diagnostics only

- `NVIDIA` PCI display devices
- `AMD` PCI display devices
- Generic PCI display adapters
- GPU `vendor/device`, `bus/slot/function`, class, command/status, IRQ, and BAR metadata
- `Intel e1000` PCI NIC presence in QEMU with experimental polling RX/TX rings
- RAM above `4 GB` is detected and reported, but not yet usable by the kernel allocator

### Not implemented yet

- Native `NVIDIA` driver, framebuffer driver, or compute acceleration
- Native `AMD` acceleration/compute path
- `AHCI`
- `NVMe`
- `USB` input/storage/network
- Full `UEFI-first` boot path
- Full network stack (`DHCP` client, robust `IPv4`, `UDP/TCP`, `SSH`)
- General-purpose use of RAM above `4 GB`
- Full GGUF inference runtime comparable to a userspace LLM engine

## Build

From WSL/Linux:

```bash
make clean
make
make test-model
make qemu
make qemu-net
make qemu-gguf
make qemu-stories15
make qemu-ext2
```

From Windows PowerShell, using WSL:

```powershell
.\scripts\run-local.ps1 -Mode mklm
.\scripts\run-local.ps1 -Mode gguf
.\scripts\run-local.ps1 -Mode stories15
.\scripts\run-local.ps1 -Mode net
.\scripts\run-local.ps1 -Mode nvidia -VfioHost 0000:01:00.0
.\scripts\run-local.ps1 -Mode ext2
```

Use `-NoRun` to only build the kernel and disk image:

```powershell
.\scripts\run-local.ps1 -Mode mklm -NoRun
```

The script expects these tools inside WSL:

```bash
sudo apt install build-essential gcc-multilib nasm qemu-system-x86 python3 dosfstools mtools e2fsprogs
```

By default the QEMU targets use `tcg`, which is the most portable accelerator
for WSL and Windows-hosted development. If your WSL distro has KVM available,
run with `-Accel kvm` or `make QEMU_ACCEL=kvm qemu`.

The NVIDIA QEMU mode is for Linux/KVM hosts with VFIO passthrough configured:

```powershell
.\scripts\run-local.ps1 -Mode nvidia -VfioHost 0000:01:00.0
```

or from WSL/Linux:

```bash
make QEMU_ACCEL=kvm VFIO_HOST=0000:01:00.0 qemu-nvidia
```

Replace `0000:01:00.0` with the PCI BDF of the GPU. This keeps QEMU's standard
VGA device as the interactive console and exposes the NVIDIA device as an
additional PCI display device so `gpu info` can report it. Normal WSL does not
provide direct PCI GPU passthrough; this path expects a Linux host with IOMMU,
VFIO binding, and permissions already configured.

## Run

Recommended QEMU command for the MKNN demo:

```bash
qemu-system-i386 -m 128M -kernel build/kernel.bin -initrd models/model.mklm -serial stdio
```

Inside MicroK:

```text
help
mem
mem map
heaptest
gpu info
net status
net config dhcp
net config static 10.0.2.15 255.255.255.0 10.0.2.2
llm net on
llm net status
ls
ls models
loadmodel model.mklm
loadmodel models/model.mklm
llm status
llm info
llm trace on
llm selftest
llm ask hola
llm ask que estado tiene el kernel
```

For GGUF smoke testing:

```text
loadmodel tin1.gguf
llm status
llm info
llm trace on
llm ask hola
```

To boot with `stories15M.gguf` loaded immediately as the initial LLM model:

```powershell
.\scripts\run-local.ps1 -Mode stories15
```

or from WSL/Linux:

```bash
make qemu-stories15
```

If a GGUF model stays in `PARSER-ONLY`, `llm info` now includes a `detail:`
line describing the missing metadata or tensor that blocked
`GENERATIVE-PREVIEW`.

## Model Files

- `models/model.mklm`: MicroK MKLM/MKNN model used by the demo.
- `models/tin1.gguf`: small GGUF reference model used by `make qemu-gguf`.
  MicroK can load it from FAT32, parse metadata/tensors, and report
  architecture details. When the minimum tensor set is present, MicroK enables
  a first-token `GENERATIVE-PREVIEW` path.

See:

- [docs/MKLM_MODEL.md](docs/MKLM_MODEL.md)
- [docs/PAE_MEMORY_PLAN.md](docs/PAE_MEMORY_PLAN.md)
- [docs/LLM_BOOT_TEST.md](docs/LLM_BOOT_TEST.md)
- [docs/FAT32_LOADMODEL_TEST.md](docs/FAT32_LOADMODEL_TEST.md)
- [docs/EXT2_READ_TEST.md](docs/EXT2_READ_TEST.md)
- [ROADMAP.md](ROADMAP.md)

## Network Config

MicroK does not have a NIC driver or DHCP client yet, but the network
configuration model is in place. The shell supports:

```text
net status
net config dhcp
net config static <ip> <netmask> <gateway>
```

At boot, MicroK tries to load a text config from FAT32:

- `/microk/net.cfg`
- `/net.cfg`

Example `net.cfg`:

```ini
mode=static
ip=10.0.2.15
netmask=255.255.255.0
gateway=10.0.2.2
dns=10.0.2.3
hostname=microk
llm_net=on
llm_port=1234
```

For DHCP mode:

```ini
mode=dhcp
hostname=microk
```

`make qemu-net` boots MicroK with a QEMU e1000 NIC, forwards host UDP port
`1234` to guest UDP port `1234`, and writes this static config into
`/microk/net.cfg` inside the FAT32 image. MicroK can detect the e1000 PCI
device, read/report its MAC address, initialize experimental RX/TX descriptor
rings, and report vendor/device/BARs in `net status`. The driver uses polling
from the shell loop; interrupts, ICMP, DHCP, TCP, and broader IPv4 routing are
still pending.

The experimental LLM service protocol is:

```text
PING
STATUS
INFO
ASK <prompt>
```

Inside MicroK:

```text
llm net on
llm net port 1234
llm net status
```

Host-side test client:

```bash
python3 scripts/llm_udp_client.py PING
python3 scripts/llm_udp_client.py ASK hola
```

Reproducible smoke test:

```bash
bash scripts/qemu_udp_smoke.sh
```

Current known state: `make qemu-net` enables the LLM UDP service from
`/microk/net.cfg`, ARP request/reply works in QEMU, and QEMU emits the UDP frame
after ARP resolution. The remaining bug is in consuming that follow-up UDP frame
from the e1000 RX ring, so `qemu_udp_smoke.sh` may still time out until RX
recycling is hardened.

## Limitations

- Scheduler is not yet robust; the stable shell path runs foreground and the PIT no longer preempts it.
- User mode isolation is experimental.
- RAM above 4GB is detected and reported as a non-allocatable high pool; PAE table helpers exist, but PAE mapping/activation is still pending and documented in `docs/PAE_MEMORY_PLAN.md`.
- FAT32/ext/NTFS support is experimental/read-only/probe-level.
- FAT32 shell navigation supports relative paths, `.` and `..`, and displays
  ASCII VFAT long names when present.
- Network configuration, e1000 PCI detection, MAC readout, experimental RX/TX rings, and a UDP LLM protocol path exist. NIC interrupts, ICMP, TCP, DHCP, and robust IPv4 routing are not implemented yet.
- Disk-loaded models still require one contiguous low-memory buffer; RAM above 4GB is diagnostic/experimental and is not used for model buffers yet.
- GGUF loading/parsing works. Some models can enter a minimal
  `GENERATIVE-PREVIEW` path that attempts the first generated token, but full
  autoregressive GGUF inference is still incomplete.
- GPU support is PCI detection only, not ROCm/compute acceleration.
- NVIDIA support is detection/diagnostics only; framebuffer and compute drivers are future work.
