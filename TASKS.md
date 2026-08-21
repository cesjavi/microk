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
- [x] Implementar loader de tensores Llama-like completo.
- [x] Implementar RMSNorm, RoPE, softmax y SwiGLU con tests.
- [ ] Implementar forward pass de una capa contra referencia Python.
- [x] Habilitar `GENERATIVE` para primer token cuando el GGUF tenga tensores minimos.
- [x] Implementar decoder completo con argmax greedy.
- [x] Agregar KV cache para contexto pequeno.
- [x] Integrar generacion real en `llm ask`.

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
- [x] FAT32: VFAT Unicode completo y checksum LFN (ver ROADMAP.md Fase 4).
- [x] ext2 read-only inicial: listar root y leer archivos directos.
- [x] ext2 read-only: subdirectorios y single indirect blocks.
- [x] ext2 read-only: double/triple indirect blocks.
- [x] Agregar prueba manual `qemu-ext2` con imagen ext2.
- [x] ext2 read-only real: validación amplia (ver ROADMAP.md Fase 4). Manejo robusto ext3/ext4 (journal replay, extents) sigue pendiente.
- [ ] USB storage (UHCI hecho; EHCI: deteccion+bring-up+puertos+enumeracion hecho en `kernel/ehci.c`, Bulk-Only Transport pendiente — ver ROADMAP.md Etapa 3).

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
 - [x] Hacer el pool alto consciente de regiones reales del mmap.
 - [x] Implementar estructuras PAE y helpers de entradas de 64 bits.
 - [x] Activar PAE en `vmm_init` con fallback si CPUID no lo soporta.
 - [x] Implementar ventanas temporales de high memory.
 - [ ] Mover buffers grandes de modelos/cache a high memory.
 - [ ] Usar RAM >4GB como memoria general del kernel.
 - [x] Syscalls básicas.
 - [x] **Spinlocks/Mutexes**: Sincronización para recursos compartidos (Video/KBD).
 - [x] **Mapeo Seguro de Modelos**: Mover la carga de modelos a regiones reservadas del PMM.
 - [x] **User Mode (Ring 3)**: Base operativa (TSS + iret jump).
 - [x] **Protección de Páginas**: Impedir que Ring 3 acceda a memoria del Kernel.
 - [x] Scheduler robusto.
 - [x] Shared memory IPC (API estilo SysV shm; ver ROADMAP.md Fase 2).

## Persistencia e IA Avanzada

- [x] **Escritura ATA (Sectores)**: Implementar `write_sector` con Cache Flush.
- [x] **Escritura en FAT32**: Implementada creación de archivos, borrado y actualización de clusters.
- [x] **Librería Matemática**: Soft-float o Fixed-point para tensores.
- [x] Parser GGUF completo.
- [x] Tokenizer.
- [x] Tensor loader.
- [x] Matmul FP32/int cuantizado.
- [x] Network stack.

## NVIDIA / GPU Real

- [x] Detectar NVIDIA por PCI.
- [x] Mostrar bus/slot/function y BARs en `gpu`.
- [x] Usar framebuffer Multiboot/VBE si existe.
- [x] Implementar consola grafica sobre framebuffer.
- [x] Mapear BAR0 MMIO NVIDIA de forma segura.
- [x] Leer registros seguros de identificacion.
- [ ] Investigar Nouveau para inicializacion y command submission.
- [ ] Posponer compute NVIDIA hasta tener MMIO, memoria GPU y colas estables.
- [ ] Planificar backend LLM sobre NVIDIA despues de driver propio de VRAM/colas; no depender de CUDA dentro del kernel.

## Red / IP Dinamica y Configurable

- [x] Definir `net_config_t` con modo DHCP/static, IP, netmask, gateway, DNS y hostname.
- [x] Agregar parser de `/microk/net.cfg` para IP estatica.
- [x] Agregar comandos `net status`, `net config dhcp`, `net config static <ip> <mask> <gw>`.
- [ ] Implementar driver e1000 inicial para QEMU.
- [x] Detectar e1000 por PCI y reportar vendor/device/BARs en `net status`.
- [x] Leer MAC address de e1000.
- [x] Implementar Ethernet RX/TX basico.
- [x] Implementar ARP.
- [x] Implementar IPv4 + ICMP echo reply.
- [x] Implementar `ping <ip>`.
- [x] Implementar UDP minimo.
- [x] Implementar cliente DHCP Discover/Offer/Request/Ack.
- [x] Implementar DNS A record resolver + `nslookup <host>`.
- [x] Agregar `make qemu-net`.
- [x] Detectar Intel Wireless-AC 9560 (PCI IDs iwlwifi 9000/CNVi) y mostrar su estado separado de Ethernet.
- [x] Seleccionar Realtek PCIe GbE `10EC:8168` (RTL8111H, `1849:8168`, REV 15) como backend Ethernet soportado.
- [x] Realtek 8168: pad TX a 60 bytes, retirar FCS RX, rechazar fragmentos y ordenar transferencias de ownership DMA.
- [x] RTL8111H: detectar XID `0x541`, aplicar configuracion RX/MaxTx especifica, limpiar estados PFM/D3cold y publicar contadores DMA.
- [x] Arrancar MicroK en la RTL8111H fisica y verificar enlace, DHCP, ping y contadores RX/TX sin errores. Verificado: link up, lease DHCP real (visible en el cliente del router), `ping` responde TTL=64 <1ms de forma estable, y UDP bidireccional (RSH `PING`->`PONG`) funciona end-to-end. Ver detalle en ROADMAP.md, "Hallazgos de la sesion de bring-up en laptop real". Contadores RX/TX explicitos sin revisar por separado (no se pudo leer `net status` sin pantalla).
- [x] Intel Wireless-AC 9560: localizar y validar firmware TLV `iwlwifi-9000-pu-b0-jf-b0-46.ucode` desde FAT32.
- [x] Intel Wireless-AC 9560: catalogar por separado secciones INIT/runtime con offsets y limites DMA.
- [x] Intel Wireless-AC 9560: preservar separadores CPU/paging y preparar staging DMA FH de 128 KiB.
- [x] Intel Wireless-AC 9560: mapear ventanas CSR/FH y capturar revision/estado inicial sin escribir al radio.
- [x] Intel Wireless-AC 9560: aceptar BAR0 de 64 bits direccionable bajo 4 GiB y rechazar solo MMIO realmente inaccesible.
- [x] Agregar firmware API 46 oficial y test estructural contra la imagen TLV real.
- [x] Intel Wireless-AC 9560: preparar RBD MQ de 512 entradas, IDs virtuales, status y buffers RX de 4 KiB.
- [x] Intel Wireless-AC 9560: adquirir propiedad PCI/MAC y programar RFH queue 0 con timeouts.
- [x] Intel Wireless-AC 9560: implementar transferencia FH de secciones en chunks de 128 KiB con completion y timeout.
- [x] Intel Wireless-AC 9560: publicar buffers RX MQ y validar la notificacion `ALIVE` con timeout y estado CAFE.
- [x] Intel Wireless-AC 9560: reservar TFD/host-command DMA, keep-warm y extraer `scd_base_ptr` desde `ALIVE`.
- [x] Intel Wireless-AC 9560: inicializar SRAM SCD, byte-count table, FIFO 7 y canales TX para command queue 0.
- [x] Intel Wireless-AC 9560: emitir `ECHO_CMD` por TFD y correlacionar su respuesta RX por secuencia.
- [x] Intel Wireless-AC 9560: arrancar INIT ucode y generalizar host commands sobre un ring circular.
- [x] Intel Wireless-AC 9560: leer secciones NVM por firmware y obtener la MAC desde CSR strap/OTP.
- [x] Intel Wireless-AC 9560: enviar TX antenna/PHY config y recolectar notificaciones PHY DB.
- [x] Intel Wireless-AC 9560: ejecutar INIT ucode, leer NVM y recolectar PHY DB/calibracion antes de runtime.
- [x] Intel Wireless-AC 9560: reiniciar transporte, cargar runtime, configurar paging y reinyectar PHY DB.
- [x] Intel Wireless-AC 9560: validar la tabla CMD_VERSIONS de API 46 (scan config/request, ADD_STA y PHY context).
- [x] Intel Wireless-AC 9560: reciclar buffers RX MQ y republicar el write pointer para operacion continua.
- [x] Intel Wireless-AC 9560: habilitar DQA, cola auxiliar SCD 1 y estacion auxiliar ADD_STA v10.
- [x] Intel Wireless-AC 9560: validar canales regulatorios NVM y crear los tres contextos PHY iniciales.
- [x] Intel Wireless-AC 9560: configurar UMAC scan, emitir scan pasivo v8 y procesar completion asincrono.
- [x] Intel Wireless-AC 9560: decodificar RX PHY/MPDU, validar CRC y listar redes por BSSID, SSID, canal, RSSI y RSN.
- [x] Intel Wireless-AC 9560: implementar contextos MAC/binding y TX/RX 802.11 para autenticacion y asociacion abierta.
- [x] Intel Wireless-AC 9560: implementar WPA2-PSK (PBKDF2/PTK, EAPOL 4-way, MIC, AES unwrap y claves CCMP/GTK).
- [ ] Intel Wireless-AC 9560: validar INIT/runtime/PHY DB sobre hardware 9560 real.
- [x] Intel Wireless-AC 9560: cargar secciones del firmware al dispositivo y levantar transporte DMA.
- [x] Intel Wireless-AC 9560: implementar scan/asociacion 802.11 y WPA2 antes de exponerla al stack IP.

## LLM Remoto por Red

- [x] Definir protocolo UDP textual: `PING`, `STATUS`, `INFO`, `ASK <prompt>`.
- [x] Implementar servicio UDP stateless para consultar `llm_inference`.
- [x] Agregar config de puerto y flag `llm_net_enabled`.
- [x] Agregar comandos `llm net on`, `llm net off`, `llm net status`.
- [x] Limitar prompt/respuesta a buffers seguros.
- [x] Agregar token simple opcional para entorno de laboratorio (prefijo <token> en cada request).
- [x] Crear `scripts/llm_udp_client.py`.
- [x] Documentar prueba host -> MicroK con `ASK hola` (RDT fix completa el ciclo).
- [x] Implementar RSH: shell remota UDP en puerto 2323 con token y flag de habilitacion.
- [ ] Posponer SSH hasta tener TCP, crypto y autenticacion robusta.
