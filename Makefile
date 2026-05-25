CC = gcc
AS = nasm
LD = ld
PYTHON = python3
QEMU = qemu-system-i386
QEMU_ACCEL ?= tcg
QEMU_ACCEL_ARGS = -accel $(QEMU_ACCEL)
VFIO_HOST ?=
QEMU_NVIDIA_DEVICE ?= vfio-pci,host=$(VFIO_HOST)

CFLAGS = -m32 -ffreestanding -O3 -Wall -Wextra -fno-stack-protector -fno-pic -msse2 -mfpmath=sse -march=pentium4 -Ilib -ffast-math
LDFLAGS = -m elf_i386 -T kernel/linker.ld

KERNEL_BIN = build/kernel.bin
OBJ = kernel/boot.o kernel/main.o kernel/boot_modules.o kernel/shell.o kernel/gdt.o kernel/idt.o kernel/interrupts.o kernel/timer.o kernel/pmm.o kernel/vmm.o kernel/vmm_pae.o kernel/ipc.o kernel/task.o kernel/syscall.o kernel/keyboard.o kernel/llm.o kernel/llm_gguf.o kernel/tensor.o kernel/tokenizer.o lib/string.o lib/math.o kernel/kheap.o kernel/vfs.o kernel/blockdev.o kernel/ata.o kernel/partition.o kernel/storage.o kernel/extfs.o kernel/ntfs.o kernel/fat32.o kernel/pci.o kernel/gpu.o kernel/net.o kernel/initrd.o kernel/video.o kernel/speaker.o kernel/font.o kernel/serial.o kernel/ai_hooks.o kernel/spinlock.o

.PHONY: all test-model clean image-mklm image-gguf image-net image-ext2 qemu qemu-gguf qemu-stories15 qemu-net qemu-nvidia qemu-ext2

all: $(KERNEL_BIN)

test-model:
	$(PYTHON) scripts/test_mklm_model.py

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel/boot.o: kernel/boot.asm
	$(AS) -f elf32 $< -o $@

kernel/interrupts.o: kernel/interrupts.asm
	$(AS) -f elf32 $< -o $@

kernel/main.o: kernel/main.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel/boot_modules.o: kernel/boot_modules.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel/shell.o: kernel/shell.c
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(OBJ)
	mkdir -p build
	$(LD) $(LDFLAGS) $(OBJ) -o $@

clean:
	rm -rf build kernel/*.o lib/*.o storage.img storage-ext2.img model.mklm net.cfg

image-mklm: $(KERNEL_BIN)
	$(PYTHON) scripts/make_mklm_model.py model.mklm --name test-model \
		--pair "hola=Hola humano. Soy la red neuronal de MicroK." \
		--pair "llamas=Soy MicroK-LLM, tu asistente bare-metal." \
		--pair "creo=Fui creado por ti durante esta fantastica sesion." \
		--pair "fat32=FAT32 es el sistema de archivos que implementamos hoy." \
		--pair "sumar=Soy un modelo de lenguaje clasificador, aun no se sumar numeros." \
		--pair "ayuda=Preguntame: hola, llamas, creo, fat32 o sumar."
	dd if=/dev/zero of=storage.img bs=1M count=512
	mkfs.fat -F 32 storage.img
	mcopy -i storage.img model.mklm ::/model.mklm
	mmd -i storage.img ::/models
	mcopy -i storage.img model.mklm ::/models/model.mklm
	for f in *.gguf; do [ -f "$$f" ] || continue; mcopy -o -i storage.img "$$f" ::/"$$f"; mcopy -o -i storage.img "$$f" ::/models/"$$f"; done

qemu: image-mklm
	$(QEMU) -kernel build/kernel.bin -drive file=storage.img,format=raw,index=0,media=disk -vga std -m 256M -serial stdio $(QEMU_ACCEL_ARGS)

image-gguf: $(KERNEL_BIN)
	$(PYTHON) scripts/make_mklm_model.py model.mklm --name gguf-smoke \
		--pair "hola=Hola desde MicroK antes de cargar GGUF." \
		--pair "gguf=Carga tin1.gguf con loadmodel tin1.gguf."
	dd if=/dev/zero of=storage.img bs=1M count=512
	mkfs.fat -F 32 storage.img
	mcopy -i storage.img model.mklm ::/model.mklm
	mmd -i storage.img ::/models
	mcopy -i storage.img model.mklm ::/models/model.mklm
	for f in *.gguf; do [ -f "$$f" ] || continue; mcopy -o -i storage.img "$$f" ::/"$$f"; mcopy -o -i storage.img "$$f" ::/models/"$$f"; done

qemu-gguf: image-gguf
	$(QEMU) -kernel build/kernel.bin -drive file=storage.img,format=raw,index=0,media=disk -vga std -m 512M -serial stdio $(QEMU_ACCEL_ARGS)

qemu-stories15: image-gguf
	$(QEMU) -kernel build/kernel.bin -initrd stories15M.gguf -drive file=storage.img,format=raw,index=0,media=disk -vga std -m 768M -serial stdio $(QEMU_ACCEL_ARGS)

image-net: $(KERNEL_BIN)
	$(PYTHON) scripts/make_mklm_model.py model.mklm --name net-test \
		--pair "hola=Hola desde MicroK con configuracion de red." \
		--pair "red=La configuracion de red esta disponible; e1000 RX/TX experimental esta activo." \
		--pair "estado=MicroK cargo net.cfg desde FAT32 si esta presente."
	printf "mode=static\nip=10.0.2.15\nnetmask=255.255.255.0\ngateway=10.0.2.2\ndns=10.0.2.3\nhostname=microk\nllm_net=on\nllm_port=1234\n" > net.cfg
	dd if=/dev/zero of=storage.img bs=1M count=512
	mkfs.fat -F 32 storage.img
	mcopy -i storage.img model.mklm ::/model.mklm
	mmd -i storage.img ::/models
	mcopy -i storage.img model.mklm ::/models/model.mklm
	for f in *.gguf; do [ -f "$$f" ] || continue; mcopy -o -i storage.img "$$f" ::/"$$f"; mcopy -o -i storage.img "$$f" ::/models/"$$f"; done
	mmd -i storage.img ::/microk
	mcopy -i storage.img net.cfg ::/microk/net.cfg

qemu-net: image-net
	$(QEMU) -kernel build/kernel.bin -initrd stories15M.gguf -drive file=storage.img,format=raw,index=0,media=disk -netdev user,id=net0,hostfwd=udp::1234-:1234 -device e1000,netdev=net0 -vga std -m 256M -serial stdio $(QEMU_ACCEL_ARGS)

qemu-nvidia: image-mklm
	test -n "$(VFIO_HOST)" || (echo "Set VFIO_HOST=<pci-bdf>, for example VFIO_HOST=0000:01:00.0"; exit 1)
	$(QEMU) -kernel build/kernel.bin -drive file=storage.img,format=raw,index=0,media=disk -vga std -m 512M -serial stdio -device $(QEMU_NVIDIA_DEVICE) $(QEMU_ACCEL_ARGS)

image-ext2: $(KERNEL_BIN)
	$(PYTHON) scripts/make_mklm_model.py model.mklm --name ext2-test \
		--pair "hola=Hola desde un modelo cargado por EXT2." \
		--pair "ext2=MicroK leyo este modelo desde un filesystem EXT." \
		--pair "estado=EXT2 read-only esta activo."
	dd if=/dev/zero of=storage-ext2.img bs=1M count=512
	mkfs.ext2 -F storage-ext2.img
	debugfs -w -R "write model.mklm /model.mklm" storage-ext2.img
	debugfs -w -R "mkdir /models" storage-ext2.img
	debugfs -w -R "write model.mklm /models/model.mklm" storage-ext2.img

qemu-ext2: image-ext2
	$(QEMU) -kernel build/kernel.bin -drive file=storage-ext2.img,format=raw,index=0,media=disk -vga std -m 256M -serial stdio $(QEMU_ACCEL_ARGS)
