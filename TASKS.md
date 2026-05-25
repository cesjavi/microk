# MicroK Tasks

## Alta Prioridad

- [x] Agregar handlers básicos de excepciones CPU: page fault, general protection fault, invalid opcode.
- [x] Mejorar diagnóstico de excepciones: CR2, error code y dump mínimo de registros.
- [x] Agregar handlers para el resto de excepciones CPU.
- [x] Decodificar bits del error code de page fault.
- [x] Separar `shell_task` y carga de módulos fuera de `kernel/main.c`.
- [x] Mantener la shell foreground estable antes de reactivar multitarea.
- [x] Agregar comando `mem` para mostrar memoria detectada/reservada.
- [x] Expandir `mem` con reservas por región, heap y módulos Multiboot.
- [x] Mejorar `kheap`: coalescing de bloques libres y validación de punteros.
- [x] Agregar tests de estrés para `kmalloc/kfree` desde comando debug.
- [x] Agregar comando `llm selftest` documentado en README.

## LLM / MKLM

- [x] Cargar modelo MKLM como módulo Multiboot/initrd.
- [x] Ejecutar backend MKNN con inferencia numérica mínima.
- [x] Exponer metadata real del modelo: vocab count, class count, model name.
- [ ] Agregar fallback entrenable desde archivo, no hardcodeado.
- [x] Expandir script `make_mklm_model.py` con tests automáticos.

## LLM Generativo Real

- [x] Corregir `llm status/info` para distinguir `GGUF parser-only` de `GGUF generative`.
- [x] Elegir y documentar un modelo GGUF tiny soportado inicialmente.
- [x] Crear fixture GGUF diminuto y target `make qemu-gguf`.
- [x] Agregar trazas `llm trace on|off|status` para tokens de entrada/salida.
- [ ] Implementar loader de tensores Llama-like completo.
- [ ] Implementar RMSNorm, RoPE, softmax y SwiGLU con tests.
- [ ] Implementar forward pass de una capa contra referencia Python.
- [x] Habilitar `GENERATIVE-PREVIEW` para primer token cuando el GGUF tenga tensores minimos.
- [ ] Implementar decoder completo con argmax greedy.
- [ ] Agregar KV cache para contexto pequeno.
- [ ] Integrar generacion real en `llm ask`.

## Storage

- [x] ATA PIO básico.
- [x] MBR/GPT básico.
- [x] FAT32 read-only básico.
- [x] FAT32: lectura por nombre 8.3 más confiable.
- [x] FAT32: manejo de errores y límites de archivo.
- [x] Agregar comando `loadmodel <archivo>` desde FAT32.
- [x] Corregir `loadmodel` para no usar buffer fijo junto al stack de usuario.
- [x] Mover `loadmodel` a una ruta kernel-side para evitar punteros/buffers de usuario.
- [x] FAT32: subdirectorios 8.3 read-only.
- [x] Agregar prueba manual documentada de `loadmodel` con imagen FAT32 QEMU.
- [x] FAT32: nombres largos VFAT ASCII básicos.
- [ ] FAT32: VFAT Unicode completo y checksum LFN.
- [x] ext2 read-only inicial: listar root y leer archivos directos.
- [x] ext2 read-only: subdirectorios y single indirect blocks.
- [x] ext2 read-only: double/triple indirect blocks.
- [x] Agregar prueba manual `qemu-ext2` con imagen ext2.
- [ ] ext2 read-only real: validación amplia y manejo robusto ext3/ext4.
- [ ] USB storage.

## Kernel y Estabilidad
 
 - [x] GDT/IDT/PIC básico.
 - [x] PMM/VMM básico.
 - [x] PMM usa Multiboot memory map para liberar solo RAM usable.
 - [x] Agregar comando `mem map`.
 - [x] Separar stats de RAM usable real vs espacio direccionable.
 - [x] Validar solapamientos y entradas corruptas del memory map.
 - [x] Diseñar PAE/long mode para RAM >4GB.
 - [x] Agregar `phys_addr_t` y flags de alloc del PMM.
 - [x] Reportar memoria alta detectada por Multiboot en `mem`.
 - [x] Separar stats de pools PMM low/high.
 - [x] Implementar asignacion real desde pool alto.
 - [x] Implementar estructuras PAE y helpers de entradas de 64 bits.
 - [x] Implementar ventanas temporales de high memory.
 - [x] Syscalls básicas.
 - [x] **Spinlocks/Mutexes**: Sincronización para recursos compartidos (Video/KBD).
 - [x] **Mapeo Seguro de Modelos**: Mover la carga de modelos a regiones reservadas del PMM.
 - [x] **User Mode (Ring 3)**: Base operativa (TSS + iret jump).
 - [x] **Protección de Páginas**: Impedir que Ring 3 acceda a memoria del Kernel.
 - [ ] Scheduler robusto.
 - [ ] Shared memory IPC.

## Persistencia e IA Avanzada

- [x] **Escritura ATA (Sectores)**: Implementar `write_sector` con Cache Flush.
- [x] **Escritura en FAT32**: Implementada creación de archivos, borrado y actualización de clusters.
- [x] **Librería Matemática**: Soft-float o Fixed-point para tensores.
- [x] Parser GGUF completo.
- [x] Tokenizer.
- [x] Tensor loader.
- [x] Matmul FP32/int cuantizado.
- [ ] Network stack.

## NVIDIA / GPU Real

- [x] Detectar NVIDIA por PCI.
- [x] Mostrar bus/slot/function y BARs en `gpu`.
- [x] Usar framebuffer Multiboot/VBE si existe.
- [x] Implementar consola grafica sobre framebuffer.
- [x] Mapear BAR0 MMIO NVIDIA de forma segura.
- [x] Leer registros seguros de identificacion.
- [ ] Investigar Nouveau para inicializacion y command submission.
- [ ] Posponer compute NVIDIA hasta tener MMIO, memoria GPU y colas estables.

## Red / IP Dinamica y Configurable

- [x] Definir `net_config_t` con modo DHCP/static, IP, netmask, gateway, DNS y hostname.
- [x] Agregar parser de `/microk/net.cfg` para IP estatica.
- [x] Agregar comandos `net status`, `net config dhcp`, `net config static <ip> <mask> <gw>`.
- [ ] Implementar driver e1000 inicial para QEMU.
- [x] Detectar e1000 por PCI y reportar vendor/device/BARs en `net status`.
- [x] Leer MAC address de e1000.
- [x] Implementar Ethernet RX/TX basico.
- [x] Implementar ARP.
- [ ] Implementar IPv4 + ICMP echo reply.
- [ ] Implementar `ping <ip>`.
- [ ] Implementar UDP minimo.
- [ ] Implementar cliente DHCP Discover/Offer/Request/Ack.
- [x] Agregar `make qemu-net`.

## LLM Remoto por Red

- [ ] Definir protocolo UDP textual: `PING`, `STATUS`, `INFO`, `ASK <prompt>`.
- [ ] Implementar servicio UDP stateless para consultar `llm_inference`.
- [ ] Agregar config de puerto y flag `llm_net_enabled`.
- [ ] Agregar comandos `llm net on`, `llm net off`, `llm net status`.
- [ ] Limitar prompt/respuesta a buffers seguros.
- [ ] Agregar token simple opcional para entorno de laboratorio.
- [ ] Crear `scripts/llm_udp_client.py`.
- [ ] Documentar prueba host -> MicroK con `ASK hola`.
- [ ] Posponer SSH hasta tener TCP, crypto y autenticacion robusta.
