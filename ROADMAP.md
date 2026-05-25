# MicroK: AI-Native OS Roadmap

MicroK es un sistema operativo experimental orientado a explorar una idea concreta:
un kernel capaz de cargar modelos, ejecutar inferencia mínima y exponer servicios
del sistema mediante una shell asistida por IA.

Estado de madurez usado en este roadmap:

- **Estable**: compila y fue probado en QEMU con flujo básico.
- **Experimental**: existe código funcional, pero todavía tiene límites fuertes.
- **Pendiente**: diseño deseado, sin implementación suficiente.

## Fase 1: Base del Kernel

- [x] **Boot Multiboot v1** - Estable.
- [x] **GDT/IDT/PIC** - Estable para IRQ0/IRQ1; otros IRQs siguen enmascarados.
- [x] **Timer PIT** - Estable como reloj básico.
- [x] **Teclado PS/2** - Estable para entrada básica, Shift incluido.
- [x] **PMM** - Experimental; bitmap funcional, reservas básicas, `phys_addr_t` inicial y stats por `mem`.
- [x] **Multiboot memory map** - Experimental; el PMM libera regiones usable del mapa, no asume RAM continua.
- [x] **KHeap básico** - Experimental; allocator simple con validación de punteros, coalescing y selftest debug.
- [x] **VMM** - Experimental; identity mapping ampliado, con diagnóstico básico de page fault.
- [ ] **High memory / PAE** - Diseñado; plan documentado en `docs/PAE_MEMORY_PLAN.md`, implementación pendiente.
- [x] **Exception handlers CPU 0-31** - Experimental; instalan stubs para excepciones CPU comunes/reservadas.
- [x] **Diagnóstico de page fault** - Experimental; reporta vector, error code, CR2, registros guardados y decodifica bits básicos.
- [ ] **Exception recovery / user kill** - Pendiente; todavía no recupera ni mata procesos de usuario.

## Fase 2: Shell, Syscalls e IPC

- [x] **Syscalls `int 0x80`** - Experimental; útiles en ring 0, sin validación user/kernel robusta.
- [x] **Shell interactiva** - Estable para demo; corre foreground como loop principal.
- [x] **Shell separada de `main.c`** - Estable para demo; comandos movidos a `kernel/shell.c`.
- [x] **Comandos básicos** - `help`, `clear`, `status`, `mem`, `mem map`, `heaptest`, `fs`, `gpu`, `gpu info`, `llm`.
- [x] **Diagnóstico de memoria por shell** - Experimental; muestra RAM direccionable/usable, validación de mmap, PMM, reservas boot/módulos y heap.
- [x] **IPC por puertos** - Experimental; cola simple de un mensaje.
- [ ] **User mode real** - Pendiente.
- [ ] **Scheduler robusto** - Pendiente; el scheduler existe, pero no se activa en la demo foreground.
- [ ] **Shared memory IPC** - Pendiente.

## Fase 3: LLM Nativo

- [x] **Carga de modelo por Multiboot/initrd** - Estable para `models/model.mklm`.
- [x] **Carga de modulos separada de `main.c`** - Estable; seleccion initrd/model movida a `kernel/boot_modules.c`.
- [x] **Formato MKLM v1** - Estable para pruebas.
- [x] **Backend MKRP de reglas** - Experimental.
- [x] **Backend MKNN neural mínimo** - Estable para demo; inferencia numérica real con features y pesos.
- [x] **Comandos LLM** - `llm status`, `llm info`, `llm test`, `llm selftest`, `llm ask`.
- [x] **Metadata MKLM en shell** - `llm info` reporta nombre, tamaño, vocab/classes o rules.
- [x] **Tests de generador MKLM** - `make test-model` valida headers y payloads MKNN/MKRP.
- [x] **Parser GGUF completo** - Estable; permite extraer metadatos y tensores específicos.
- [x] **Tokenizer real** - Estable; permite codificar y decodificar tokens desde metadatos GGUF.
- [x] **Carga de tensores GGUF** - Estable; permite cargar y convertir datos a punto fijo.
- [x] **Matmul / backend tensorial** - Estable; operaciones de matrices y activaciones (ReLU/Sigmoid/Tanh).
- [ ] **Generacion de texto real tipo Llama/Qwen** - Pendiente; parser/tokenizer/tensores existen, pero falta el forward pass transformer completo.

### Roadmap de Inferencia LLM Generativa

Objetivo: pasar de MKRP/MKNN como reglas/clasificador a un backend GGUF generativo
capaz de responder prompts nuevos mediante prediccion autoregresiva de tokens.

#### Etapa 0: Baseline honesto

- [x] Detectar y cargar modulos MKLM/GGUF desde boot/storage.
- [x] Parsear headers, metadata y tensores GGUF.
- [x] Tokenizer GGUF basico.
- [x] Operaciones tensoriales minimas.
- [x] Separar claramente en UI/estado: `MKRP`, `MKNN`, `GGUF parser-only`, `GGUF generative`.
- [ ] Agregar tests que fallen explicitamente si GGUF solo tokeniza pero no genera.

#### Etapa 1: Modelo objetivo minimo

- [x] Elegir un unico formato inicial: Llama-like GGUF tiny, preferentemente FP32/F16 sin cuantizacion.
- [x] Documentar arquitectura soportada: `n_vocab`, `n_embd`, `n_layer`, `n_head`, `n_kv_head`, `rope_freq_base`, `context_length`.
- [x] Rechazar modelos no soportados con error claro en `llm info`.
- [x] Crear un modelo fixture diminuto para QEMU, menor a 16-32 MB si es posible.
- [x] Agregar `make qemu-gguf` con imagen FAT32 que cargue ese fixture.

#### Etapa 2: Carga de tensores completa

- [x] Resolver nombres reales de tensores Llama GGUF: token embeddings, output norm, output projection, attention y FFN por capa.
- [x] Soportar shapes 1D/2D usados por Llama.
- [ ] Implementar loader con vistas sobre el modelo cuando sea seguro, evitando copiar tensores gigantes al heap.
- [x] Validar offsets, alineacion y tamanos para no leer fuera del modulo.
- [x] Reportar memoria requerida, memoria libre total y mayor bloque contiguo antes de cargar modelos desde disco.

#### Etapa 3: Matematica base

- [x] Implementar RMSNorm.
- [x] Implementar SiLU/SwiGLU.
- [x] Implementar softmax estable para logits de atencion.
- [x] Implementar RoPE para Q/K.
- [x] Mejorar `fixed_t` o agregar FP32 software segun rendimiento/precision observada.
- [x] Agregar tests host-side para cada primitiva matematica contra vectores conocidos.

#### Etapa 4: Forward pass de una capa

- [x] Token embedding: convertir token id a vector hidden.
- [x] Attention de una capa: Q/K/V/O projections.
- [x] Aplicar RoPE a Q/K.
- [x] Calcular scores, softmax y mezcla de V.
- [x] FFN: gate/up/down projections con SwiGLU.
- [x] Residual connections y normalizaciones.
- [ ] Testear una capa contra una implementacion Python de referencia con pesos pequenos.

#### Etapa 5: Decoder completo

- [x] Ejecutar `n_layer` capas en secuencia.
- [x] Implementar `output_norm` y `output` projection (logits).
- [x] Argmax basico para seleccion de token.
- [x] Bucle autoregresivo completo (generar multiples tokens).
- [x] Implementar loop autoregresivo `prompt -> next token -> append -> repeat`.
- [x] Agregar limite de tokens, stop token y timeout para evitar cuelgues.
- [x] Habilitar `GENERATIVE-PREVIEW` cuando el modelo GGUF tenga arquitectura y tensores minimos para primer token.

#### Etapa 6: KV cache y Rendimiento (PRÓXIMO PASO)

- [x] Implementar **KV cache por capa** para evitar recalcular todo el contexto en cada token.
- [x] Soporte para inferencia incremental (generación de texto fluida).
- [x] Gestión de memoria para contextos largos.

#### Etapa 7: Cuantización y Formatos (DONE)

- [x] Soporte para **Q4_0** (ahorro de 8x en RAM).
- [x] Implementar dequantizacion por bloque con validación de escala.
- [x] Carga dinámica de modelos desde FAT32 via `loadmodel`.
- [x] Carga optimizada (lectura secuencial de clusters $O(N)$).

#### Etapa 8: Interfaz y UX (DONE)

- [x] Comando `cd` y navegación de archivos real.
- [x] **Autocompletado con TAB** (insensible a mayúsculas/minúsculas).
- [x] **Barra de Progreso** visual durante la carga.
- [x] Señal de aborto (**Ctrl + C**) para operaciones largas.

#### Etapa 9: Estabilidad y Red (DONE)

- [x] **AI Remote Service**: Consultas remotas vía UDP (puerto 1234).
- [x] Corrección de arquitectura de Syscalls (retorno en EAX y protección de stack).
- [x] Validación robusta de cabeceras GGUF.

#### Etapa 10: Seguridad y Estabilidad Pro

- [ ] Validar punteros user/kernel en syscalls LLM.
- [ ] Proteger modelo cargado como read-only.

#### Etapa 10: Criterio de "LLM real"

Se considera alcanzado cuando:

- [ ] Un GGUF tiny responde a prompts no hardcodeados.
- [ ] La respuesta se genera token por token.
- [ ] El codigo ejecuta embeddings, attention, FFN, normalizacion y sampler.
- [ ] `llm ask` no depende de pares de reglas ni features MKNN.
- [ ] Existe una prueba reproducible en QEMU documentada.

## Fase 4: Storage y Model Loading

- [x] **VFS base** - Experimental.
- [x] **Block devices base** - Experimental.
- [x] **ATA PIO primary master** - Experimental; soporte de lectura y escritura (PIO).
- [x] **MBR/GPT parser básico** - Experimental.
- [x] **FAT32 write básico** - Estable; creación, expansión y borrado.
- [x] **FAT32 read-only básico** - Experimental; útil para cargar `model.mklm`.
- [x] **FAT32 lectura 8.3/VFAT básico, subdirectorios y `loadmodel`** - Experimental; busca archivos/rutas y carga modelos desde disco con ruta kernel-side y buffer contiguo asignado por PMM.
- [x] **ext2/ext3/ext4 probe** - Experimental; detecta superblock.
- [x] **ext2 read-only inicial** - Experimental; lista root y lee archivos con direct/single/double/triple indirect blocks.
- [x] **ext2 path traversal básico** - Experimental; `extcat dir/file` resuelve subdirectorios simples.
- [x] **EXT model loading** - Experimental; `loadmodel` y autoload prueban EXT si FAT32 falla.
- [x] **NTFS probe** - Experimental; detecta firma, no navega archivos.
- [ ] **FAT32 read-only robusto** - Pendiente; falta VFAT Unicode/checksum y validación más amplia.
- [ ] **ext2 read-only real** - Pendiente; falta validación amplia y manejo robusto ext3/ext4.
- [ ] **USB Mass Storage** - Pendiente.

## Fase 5: Estabilidad y Persistencia (NUEVO FOCO)

- [x] **Sincronización (Spinlocks/Mutexes)** - Estable; protege video y recursos críticos.
- [x] **User Mode (Ring 3) e Isolation** - Base operativa (TSS + iret).
- [x] **Protección de Páginas (Memoria aislada)** - Estable; Kernel protegido de Ring 3.
- [x] **Escritura en FAT32/ext2** - Estable; soporte completo de archivos y directorios.
- [x] **Librería Matemática (Fixed-point/Soft-float)** - Experimental; Q16.16 para tensores y activaciones.

## Fase 6: Video, GPU y Hardware

- [x] **VGA text mode** - Estable.
- [x] **PCI scan** - Experimental.
- [x] **GPU detection NVIDIA/AMD/generic** - Experimental.
- [ ] **Framebuffer gráfico real** - Pendiente/experimental.
- [ ] **Driver GPU compute** - Pendiente.
- [ ] **Network stack** - Pendiente.

### Roadmap de RAM Completa

Objetivo: usar toda la RAM que el bootloader declare disponible, sin pisar huecos
reservados por firmware, MMIO, ACPI, PCI BARs, kernel, modulos o bitmap.

- [x] Leer Multiboot memory map (`mmap_addr`/`mmap_length`).
- [x] Liberar en PMM solo entradas `type=1` usable.
- [x] Mantener reservado todo lo que no aparezca como usable.
- [x] Reservar kernel, modulos, bitmap y heap despues de liberar regiones usable.
- [x] Mostrar en `mem` cantidad usable real vs espacio direccionable.
- [x] Listar regiones del memory map desde shell (`mem map`).
- [x] Validar solapamientos y entradas corruptas del mmap.
- [x] Diseñar estrategia de memoria alta con PAE o migracion a long mode.
- [x] Agregar `phys_addr_t`, flags de alloc y tracking de memoria alta reportada por Multiboot.
- [x] Separar stats de pools PMM low/high; high pool detectado pero no asignable sin PAE.
- [ ] Implementar asignacion real desde pool alto para buffers grandes.
- [x] Implementar estructuras PAE y helpers de entradas de 64 bits.
- [ ] Implementar ventanas temporales reales de high memory sobre PAE.
- [ ] Soportar memoria alta con PAE para RAM >4GB como memoria general.

### Roadmap NVIDIA

Objetivo: empezar por soporte de dispositivo NVIDIA realista y seguro. Compute
NVIDIA completo no es un primer hito: requiere inicializacion compleja, firmware,
colas, memoria GPU, command submission y un modelo de driver comparable a Nouveau.

#### Etapa 0: Deteccion y diagnostico

- [x] Detectar vendor NVIDIA por PCI.
- [x] Reportar vendor/device en `gpu`.
- [x] Mostrar bus/slot/function y BARs en `gpu`.
- [x] Diferenciar VGA compatible, framebuffer BAR y MMIO BAR.
- [x] Agregar `gpu info` con IDs y BARs.

#### Etapa 1: Framebuffer antes que compute

- [x] Usar framebuffer Multiboot/VBE si esta disponible.
- [x] Mapear framebuffer como memoria write-combining si el VMM lo permite.
- [x] Implementar consola grafica simple sobre framebuffer.
- [x] Mantener VGA text mode como fallback.

#### Etapa 2: MMIO NVIDIA minimo

- [x] Mapear BAR0 MMIO de NVIDIA.
- [x] Leer registros seguros de identificacion cuando aplique.
- [x] No enviar comandos a la GPU hasta tener modelo de init claro.
- [ ] Documentar GPUs probadas y IDs soportados.

#### Etapa 3: Aceleracion/compute futura

- [ ] Estudiar Nouveau como referencia tecnica.
- [ ] Definir ABI interna para buffers GPU.
- [ ] Inicializar memoria GPU de forma segura.
- [ ] Implementar colas y command submission.
- [ ] Evaluar kernels de compute simples antes de cualquier backend LLM.

### Roadmap de Red: IP Configurable y Dinamica

Objetivo: que MicroK pueda levantar una interfaz de red en QEMU/hardware, obtener
una IP por DHCP cuando exista servidor, o usar una IP estatica configurada por el
usuario para entornos simples y pruebas reproducibles.

#### Etapa 0: Modelo de configuracion

- [x] Definir `net_config_t`: modo `dhcp`/`static`, IP, gateway, netmask, DNS y hostname.
- [x] Agregar valores por defecto seguros: DHCP habilitado, sin gateway si falla DHCP.
- [x] Permitir IP estatica por archivo de config en FAT32: `/microk/net.cfg`.
- [x] Permitir override por comando de shell: `net config static <ip> <mask> <gw>`.
- [x] Agregar comando `net config dhcp`.

#### Etapa 1: NIC inicial para QEMU

- [x] Elegir driver inicial: e1000 para QEMU.
- [x] Detectar la NIC por PCI y mostrar vendor/device en `net status`.
- [x] Leer MAC address desde e1000 MMIO/EEPROM.
- [x] Implementar RX/TX basico con buffers alineados y ownership claro.
- [x] Agregar target `make qemu-net` con parametros QEMU documentados.

#### Etapa 2: Ethernet + ARP

- [x] Definir structs Ethernet II.
- [x] Enviar y recibir frames.
- [x] Implementar ARP request/reply.
- [x] Mantener cache ARP minima.
- [x] Agregar comando `arp` o `net arp`.

#### Etapa 3: IPv4 minimo

- [ ] Definir IPv4 header, checksum y validacion.
- [ ] Recibir paquetes IPv4 dirigidos a la IP local.
- [ ] Enviar paquetes IPv4 a IP local o via gateway.
- [ ] Implementar ICMP echo reply.
- [ ] Agregar `ping <ip>` como primer test end-to-end.

#### Etapa 4: IP estatica configurable

- [x] Parser de `/microk/net.cfg` con claves: `mode`, `ip`, `netmask`, `gateway`, `dns`.
- [x] Aplicar config al boot si existe.
- [x] Exponer `net status` con MAC, modo, IP, netmask, gateway y link.
- [ ] Guardar cambios de shell en FAT32 cuando el FS permita escritura.
- [ ] Tests de parser host-side.

#### Etapa 5: DHCP cliente

- [ ] Implementar UDP minimo.
- [ ] Implementar DHCP Discover/Offer/Request/Ack.
- [ ] Configurar IP, netmask, gateway, DNS y lease time desde DHCP.
- [ ] Reintentos con timeout y fallback a config estatica si existe.
- [ ] Comando `dhcp renew`.

#### Etapa 6: DNS y utilidades

- [ ] Resolver DNS A record basico.
- [ ] Agregar `nslookup <host>`.
- [ ] Preparar base para HTTP simple o descarga de modelos.

#### Etapa 7: Servicio remoto directo al LLM

Prioridad aprobada: antes de SSH, MicroK debe poder hablar directamente con su
servicio LLM por red. SSH queda para una etapa posterior porque requiere TCP
robusto, criptografia, claves, random seguro y autenticacion madura.

- [x] Definir protocolo textual minimo: `PING`, `STATUS`, `INFO`, `ASK <prompt>`.
- [ ] Implementar servicio UDP stateless para `llm ask` end-to-end; TX y ARP reply existen, falta consumir el UDP posterior desde RX.
- [x] Agregar puerto configurable para el servicio LLM.
- [x] Limitar tamano de prompt y respuesta para evitar overflow.
- [ ] Agregar token simple opcional para laboratorio: `AUTH <token>` o prefijo compartido.
- [x] Agregar comando `llm net on/off/status` y `llm net port <puerto>`.
- [x] Crear script host-side `scripts/llm_udp_client.py` para probar desde el host.
- [x] Documentar prueba QEMU smoke: host envia `PING`, ARP responde, UDP RX posterior queda como bug abierto.
- [ ] Documentar prueba QEMU final: host envia `ASK hola`, MicroK responde por UDP.

#### Etapa 8: Shell remota liviana

- [ ] Implementar consola remota tipo telnet-like solo para QEMU/lab.
- [ ] Reusar parser de comandos de shell sin duplicar logica.
- [ ] Protegerla detras de flag/config explicito.
- [ ] Agregar autenticacion simple por token antes de aceptar comandos.
- [ ] Mantener SSH como objetivo futuro, no como dependencia temprana.

#### Criterio de listo

- [ ] `make qemu-net` levanta MicroK con NIC visible.
- [ ] `net status` muestra NIC, MAC e IP.
- [ ] Con DHCP disponible, MicroK obtiene IP automaticamente.
- [ ] Sin DHCP, MicroK usa `/microk/net.cfg` o config manual.
- [ ] `ping <gateway>` responde en QEMU.
- [ ] Un cliente UDP externo puede enviar `ASK <prompt>` y recibir respuesta del LLM.

## Prioridad Actual

- [x] **Escritura en Disco (ATA)**: Soporte PIO con Cache Flush.
- [x] **Escritura en FAT32/ext2** - Estable; Soporte para creación y expansión de archivos y actualización de clusters.
- [x] **User Mode (Ring 3)**: Shell aislado ejecutandose en Ring 3 con syscalls.
- [x] **Librería Matemática**: Implementada librería Q16.16 con exp/tanh/sigmoid para el backend de tensores.

## Norte del Proyecto

MicroK no busca solo ser una demo, sino un runtime bare-metal resiliente donde la IA
pueda operar con seguridad y persistencia real.
