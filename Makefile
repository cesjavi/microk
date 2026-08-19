CC = gcc
AS = nasm
LD = ld
PYTHON = python3
QEMU = qemu-system-i386
QEMU_ACCEL ?= tcg
QEMU_ACCEL_ARGS = -accel $(QEMU_ACCEL)
VFIO_HOST ?=
QEMU_NVIDIA_DEVICE ?= vfio-pci,host=$(VFIO_HOST)

CFLAGS = -m32 -ffreestanding -O3 -Wall -Wextra -fno-stack-protector -fno-pic -mstackrealign -msse2 -mfpmath=sse -march=pentium4 -Ilib -ffast-math
LDFLAGS = -m elf_i386 -T kernel/linker.ld

KERNEL_BIN = build/kernel.bin
OBJ = kernel/boot.o kernel/main.o kernel/boot_modules.o kernel/shell.o kernel/gdt.o kernel/idt.o kernel/interrupts.o kernel/timer.o kernel/pmm.o kernel/vmm.o kernel/vmm_pae.o kernel/highmem.o kernel/ipc.o kernel/task.o kernel/syscall.o kernel/keyboard.o kernel/llm.o kernel/llm_gguf.o kernel/tensor.o kernel/tokenizer.o lib/string.o lib/math.o kernel/kheap.o kernel/vfs.o kernel/blockdev.o kernel/ata.o kernel/uhci.o kernel/partition.o kernel/storage.o kernel/extfs.o kernel/ntfs.o kernel/fat32.o kernel/pci.o kernel/gpu.o kernel/wpa2_crypto.o kernel/iwlwifi.o kernel/net.o kernel/initrd.o kernel/video.o kernel/speaker.o kernel/font.o kernel/serial.o kernel/ai_hooks.o kernel/spinlock.o

.PHONY: all test-model test-netcfg test-iwlwifi test-wpa2 clean image-mklm image-gguf image-net image-ext2 image-usb iso qemu qemu-gguf qemu-stories15 qemu-net qemu-nvidia qemu-highmem qemu-ext2 qemu-usb

all: $(KERNEL_BIN)

test-model:
	$(PYTHON) scripts/test_mklm_model.py

test-netcfg:
	$(CC) -Wall -Wextra -o build/net_config_parser_test scripts/net_config_parser_test.c
	./build/net_config_parser_test

test-iwlwifi:
	$(PYTHON) scripts/test_iwlwifi_firmware.py

test-wpa2:
	$(CC) -O2 -Wall -Wextra -Ilib -Ikernel -o build/wpa2_crypto_test scripts/wpa2_crypto_test.c kernel/wpa2_crypto.c
	./build/wpa2_crypto_test

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
	rm -rf build kernel/*.o lib/*.o storage.img storage-ext2.img usbstick.img model.mklm net.cfg microk.iso

ISO_DIR = build/iso
KERNEL_BIOS_BIN = build/kernel-bios.bin
BIOS_OBJ = kernel/boot_bios.o $(filter-out kernel/boot.o,$(OBJ))

# Built with MICROK_NO_VIDEO_MODE (see kernel/boot.asm): GRUB on
# VirtualBox/real BIOS chokes on the normal build's VBE mode request, so the
# ISO gets its own kernel binary with that request dropped. QEMU keeps using
# the regular $(KERNEL_BIN) via -kernel, unaffected.
kernel/boot_bios.o: kernel/boot.asm
	$(AS) -f elf32 -DMICROK_NO_VIDEO_MODE $< -o $@

$(KERNEL_BIOS_BIN): $(BIOS_OBJ)
	mkdir -p build
	$(LD) $(LDFLAGS) $(BIOS_OBJ) -o $@

# GRUB2 rescue ISO for VirtualBox / real BIOS-CSM hardware (no -kernel flag
# there, unlike QEMU) - see "Roadmap de Arranque en Hardware Real" in
# ROADMAP.md. GRUB chainloads kernel-bios.bin as plain Multiboot v1, no
# further changes needed. Requires grub-pc-bin + xorriso (grub-mkrescue) on
# the host building the ISO; not needed just to run `make qemu*`.
iso: $(KERNEL_BIOS_BIN)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIOS_BIN) $(ISO_DIR)/boot/kernel.bin
	cp firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode $(ISO_DIR)/boot/iwlwifi-9000-pu-b0-jf-b0-46.ucode
	printf 'set timeout=0\nset default=0\n\nmenuentry "MicroK" {\n\tmultiboot /boot/kernel.bin\n\tmodule /boot/iwlwifi-9000-pu-b0-jf-b0-46.ucode iwlwifi-api46\n\tboot\n}\n' > $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o microk.iso $(ISO_DIR)

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
	# The network image boots its model through -initrd. Do not copy arbitrary
	# local GGUF files: a 638MB model would overflow this 512MB FAT image.
	mmd -i storage.img ::/microk
	mcopy -i storage.img net.cfg ::/microk/net.cfg
	mcopy -i storage.img firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode ::/microk/iwlwifi-9000-pu-b0-jf-b0-46.ucode

qemu-net: image-net
	$(QEMU) -kernel build/kernel.bin -initrd stories15M.gguf -drive file=storage.img,format=raw,index=0,media=disk -netdev user,id=net0,hostfwd=udp::1234-:1234 -device e1000,netdev=net0 -vga std -m 256M -serial stdio $(QEMU_ACCEL_ARGS)

qemu-nvidia: image-mklm
	test -n "$(VFIO_HOST)" || (echo "Set VFIO_HOST=<pci-bdf>, for example VFIO_HOST=0000:01:00.0"; exit 1)
	$(QEMU) -kernel build/kernel.bin -drive file=storage.img,format=raw,index=0,media=disk -vga std -m 512M -serial stdio -device $(QEMU_NVIDIA_DEVICE) $(QEMU_ACCEL_ARGS)

qemu-highmem: image-mklm
	$(QEMU) -kernel build/kernel.bin -drive file=storage.img,format=raw,index=0,media=disk -vga std -m 6G -serial stdio $(QEMU_ACCEL_ARGS)

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

image-usb: $(KERNEL_BIN)
	dd if=/dev/zero of=usbstick.img bs=1M count=64
	mkfs.fat -F 32 usbstick.img

qemu-usb: image-mklm image-usb
	$(QEMU) -kernel build/kernel.bin -drive file=storage.img,format=raw,index=0,media=disk -usb -drive if=none,format=raw,file=usbstick.img,id=usbstick -device usb-storage,drive=usbstick -vga std -m 256M -serial stdio $(QEMU_ACCEL_ARGS)
