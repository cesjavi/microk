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
- [x] **High memory / PAE base** - Experimental; `vmm_init` activa PAE si CPUID lo soporta, mantiene fallback legacy, y `highmemtest` usa ventana temporal para páginas >4GB.
- [x] **Exception handlers CPU 0-31** - Experimental; instalan stubs para excepciones CPU comunes/reservadas.
- [x] **Diagnóstico de page fault** - Experimental; reporta vector, error code, CR2, registros guardados y decodifica bits básicos.
- [x] **Exception recovery / user kill** - Experimental; fault_handler detecta CS del IRET frame para distinguir ring-3 de ring-0; faults de usuario matan la tarea y hacen task_switch en lugar de kernel panic.
- [x] **Guardado de estado FPU/SSE en interrupciones** - Crítico; ninguna ISR (`irq0`/`irq1`/`isr128`) guardaba/restauraba XMM/x87, solo `pusha`/`popa` (enteros). Con `-msse2 -mfpmath=sse` global, CUALQUIER interrupción (el timer dispara ~100/s, con o sin cambio de tarea) podía pisar registros XMM en uso por código interrumpido — se manifestaba como corrupción intermitente de cómputos float (`h_sumsq` saltando a basura tipo overflow de int32 a mitad del forward pass del LLM, visible sobre todo en modelos grandes/lentos como TinyLlama-1.1B que cruzan múltiples ticks de timer por capa; stories15M es chico y rápido, raramente lo pisaba). Fix: macros `SAVE_FPU`/`RESTORE_FPU` con `FXSAVE`/`FXRSTOR` (alineado a 16 bytes, guardado en la propia pila de la tarea para sobrevivir correctamente un `task_switch()`) agregadas en `irq0`, `irq1` e `isr128` (`kernel/interrupts.asm`). También se agregó la habilitación explícita de `CR0.MP`/`CR4.OSFXSR` en el arranque (`kernel/boot.asm`) como salvaguarda, ya que `FXSAVE`/`FXRSTOR` requieren `CR4.OSFXSR=1` para no causar `#UD` en hardware real (el resto de instrucciones SSE ya funcionaban sin esto, probablemente por permisividad de QEMU TCG).

## Fase 2: Shell, Syscalls e IPC

- [x] **Syscalls `int 0x80`** - Experimental; útiles en ring 0, sin validación user/kernel robusta.
- [x] **Shell interactiva** - Estable para demo; corre foreground como loop principal.
- [x] **Shell separada de `main.c`** - Estable para demo; comandos movidos a `kernel/shell.c`.
- [x] **Comandos básicos** - `help`, `clear`, `status`, `mem`, `mem map`, `heaptest`, `fs`, `gpu`, `gpu info`, `llm`.
- [x] **Diagnóstico de memoria por shell** - Experimental; muestra RAM direccionable/usable, validación de mmap, PMM, reservas boot/módulos y heap.
- [x] **IPC por puertos** - Experimental; cola simple de un mensaje.
- [ ] **User mode real** - Pendiente.
- [x] **Scheduler robusto** - Experimental; timer IRQ0 llama task_switch() cada 10 ticks (100ms timeslice); tarea background net_poll_task() creada antes del salto a ring-3.
- [x] **Shared memory IPC** - Experimental; API estilo SysV shm (`ipc_shm_create/attach/detach/destroy`) en `kernel/ipc.c`, expuesta por syscalls 54-58 y comando `ipc shmtest`. Nota: como `task_switch()` no cambia CR3 (un solo page directory para todas las tareas), todavía no hay aislamiento real de address space que mapear selectivamente — esto da el manejo de key/id/refcount validado, no aislamiento; eso requiere "User mode real" (per-task VMM) primero.

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
- [x] **Generacion de texto real tipo Llama/Qwen** - Experimental; forward pass Llama completo con KV cache, Q4_0 dequant, argmax greedy, bucle autoregresivo de hasta 48 tokens.

### Roadmap de Inferencia LLM Generativa

Objetivo: pasar de MKRP/MKNN como reglas/clasificador a un backend GGUF generativo
capaz de responder prompts nuevos mediante prediccion autoregresiva de tokens.

#### Etapa 0: Baseline honesto

- [x] Detectar y cargar modulos MKLM/GGUF desde boot/storage.
- [x] Parsear headers, metadata y tensores GGUF.
- [x] Tokenizer GGUF basico.
- [x] Operaciones tensoriales minimas.
- [x] Separar claramente en UI/estado: `MKRP`, `MKNN`, `GGUF parser-only`, `GGUF generative`.
- [x] Agregar tests que fallen explicitamente si GGUF solo tokeniza pero no genera — `llm selftest gguf` devuelve PASS/FAIL/SKIP según si el backend genera tokens reales.

#### Etapa 1: Modelo objetivo minimo

- [x] Elegir un unico formato inicial: Llama-like GGUF tiny, preferentemente FP32/F16 sin cuantizacion.
- [x] Documentar arquitectura soportada: `n_vocab`, `n_embd`, `n_layer`, `n_head`, `n_kv_head`, `rope_freq_base`, `context_length`.
- [x] Rechazar modelos no soportados con error claro en `llm info`.
- [x] Crear un modelo fixture diminuto para QEMU, menor a 16-32 MB si es posible.
- [x] Agregar `make qemu-gguf` con imagen FAT32 que cargue ese fixture.

#### Etapa 2: Carga de tensores completa

- [x] Resolver nombres reales de tensores Llama GGUF: token embeddings, output norm, output projection, attention y FFN por capa.
- [x] Soportar shapes 1D/2D usados por Llama.
- [x] Implementar loader con vistas sobre el modelo cuando sea seguro, evitando copiar tensores gigantes al heap — `tensor_load_gguf_view` usado para pesos de atención/FFN; token_embd y output accedidos fila a fila sin copia.
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
- [ ] Testear una capa contra una implementacion Python de referencia con pesos pequenos — pendiente fuera del scope de kernel.

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
- [x] Heap del kernel fijo en 4MB (`kernel/kheap.c`, `HEAP_SIZE`) — insuficiente para modelos más grandes que stories15M: el vocabulario del tokenizer (~900KB-1MB para 32K tokens, un kmalloc por string), el cache K/V (escala con n_layer×n_kv_head, ej. ~2.8MB en TinyLlama-1.1B con 22 capas) y las copias de pesos de normalización juntos superan 4MB antes de llegar a reservar los buffers FPU de generación, lo que se manifestaba como `LLM: Failed to allocate FPU buffer.` sin más contexto. Subido a 32MB; `kheap_init()` corre antes de `boot_modules_load()`, así que la reserva no compite con dónde se ubica el buffer del modelo.
- [x] Comando `llm chat <prompt>` — envuelve el prompt con el template de chat (`<|user|>...</s><|assistant|>`) por código, sin que el usuario tenga que tipear `<`/`|`/`>`. Necesario porque el driver de teclado (`kernel/keyboard.c`) solo tiene tabla de scancodes US sin soporte AltGr/extendido, así que esos caracteres pueden no ser tipeables en layouts no-US (ej. español/latinoamericano); además QEMU con display SDL/GTK plano no soporta pegar desde el clipboard del host sin SPICE. Modelos chat-tuneados (ej. TinyLlama-1.1B-Chat) producen salida errática sin este template.
- [x] **Causa raíz real de la explosión en TinyLlama (no eran las interrupciones)**: `n_ff` se leía de `ffn_gate.weight->dims[0]` (la dimensión de ENTRADA, n_embd=2048) en vez de `dims[1]` (el ancho real de la capa intermedia FFN, 5632 para TinyLlama). El matmul que llena `llm_ctx.ffn_gate` usa `dims[1]` correctamente (5632 valores válidos), pero el loop de SwiGLU y la copia a `ffn_gate_float` truncaban a 2048 — la proyección hacia abajo (`ffn_down`, que sí espera 5632 elementos) leía memoria del heap sin inicializar para los 3584 restantes. Esa basura es consistente entre corridas (mismo patrón de reuso de `kmalloc`), por eso el bug parecía 100% determinístico y el fix de FXSAVE (necesario y correcto de todos modos, pero no la causa de ESTE síntoma) no lo arregló. Fix: `n_ff` ahora usa `dims[1]` con el mismo fallback que `llama_matmul_fpu`, más una guarda que aborta en vez de desbordar el buffer si algún modelo futuro excede `n_embd*4`.
- [x] **Camino rápido fusionado para Q6_K** — antes, cualquier tensor Q6_K (ej. `output.weight` en TinyLlama) pasaba por el camino lento de `tensor_read_gguf_row_into` (dequantiza la fila completa a un buffer, después un loop separado hace el producto punto), ejecutado para las 32000 entradas de vocabulario en cada token generado. Agregado `dot_product_q6_k_float` (dequant + multiply-accumulate fusionados en un solo paso, mismo enfoque que `dot_product_q4_0_float`), conectado en `llama_matmul_fpu`, el loop de generación y `llm_dump_top_logits`.
- [x] **Soporte Q6_K** — TinyLlama-1.1B-Chat Q4_0.gguf (descubierto al probar un modelo con GQA real) mantiene `output.weight` en Q6_K por calidad aunque el resto del modelo sea Q4_0 (convención común de cuantizaciones legacy de llama.cpp). Agregado `dequantize_q6_k` en `kernel/tensor.c` (super-bloques de 256 elementos/210 bytes: 128B quants de 4 bits bajos, 64B de 2 bits altos, 16B escalas int8 por sub-bloque de 16, 2B escala f16 de super-bloque — replica bit a bit `dequantize_row_q6_K` de ggml), conectado en `tensor_read_gguf_row_into` y `tensor_load_gguf`. Confirmado el tipo real vía `scripts/gguf_inspect_types.py` (inspector standalone de tensores GGUF) antes de implementar, en vez de adivinar.
- [x] **Soporte GQA (Grouped-Query Attention)** — antes el código asumía `n_kv_head == n_head` siempre: el cache K/V se guardaba con stride `n_embd` y cada cabeza de Query leía su propio offset directamente, lo cual es incorrecto en cuanto `n_kv_head < n_head` (ej. TinyLlama-1.1B: 32 cabezas Q comparten 4 cabezas KV) — hubiera producido la misma basura numérica que depuramos con stories15M. Fix en `kernel/llm.c`: `llm_effective_n_kv_head()` lee `n_kv_head` del metadata GGUF con fallback seguro a MHA si falta o es inválido; el cache K/V ahora usa stride `kv_dim = n_kv_head*head_dim`; cada cabeza de Query `h` mapea a la cabeza KV `h / (n_head/n_kv_head)`. Los buffers de trabajo `llm_ctx.k`/`v` se mantienen anchos `n_embd` sin cambios (se reusan para la proyección O y FFN-down, que sí son de ancho `n_embd`).

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

- [x] Validar punteros user/kernel en syscalls LLM — `vmm_user_ptr_ok()` comprueba U/S=1 en PDE y PTE; guardas `uptr_ok`/`ustr_ok` en todos los syscalls que aceptan punteros de usuario (casos 1, 3-7, 13-14, 16-19, 21-22, 25-27, 30, 35-36, 39, 42, 45-47, 50-51).
- [x] Proteger modelo cargado como read-only — `llm_load_model_module()` remarca las páginas del modelo como supervisor read-only tras reservar el rango, y la syscall legacy valida el buffer antes de registrarlo.

#### Etapa 10: Fix de calidad/rendimiento en generación GGUF real (stories15M)

- [x] Cachear `gguf_probe` por (start,size) — evitaba re-escanear todo el metadata (~32K tokens) en cada lookup de tensor, causa real de la lentitud al generar con stories15M.
- [x] Relajar `llm_preview_token_usable`: ya no descarta tokens cortos como "a", "is", "to", "in" del argmax.
- [x] Escanear vocabulario completo (`LLM_GGUF_PREVIEW_VOCAB_LIMIT` 4096 -> 65536) en vez de solo los primeros 4096 tokens.
- [x] Mover el lookup del tensor de salida fuera del loop de generación (una vez por inferencia, no por token).
- [x] Decodificar el marcador SentencePiece de espacio (▁, UTF-8 `E2 96 81`) que usan stories15M/TinyStories — sin esto, todo token de inicio de palabra parecía no-ASCII y quedaba excluido del argmax, dejando solo fragmentos de subpalabra pegados ("erazinenseparogonzo").
- [x] Relajar el límite adaptativo de tokens generados (`llm_gguf_generation_token_limit`): 8→32 para modelos tipo stories15M (n_embd>=256), ya no hacía falta ese límite tan bajo tras cachear `gguf_probe`.
- [x] Corregir convención de RoPE en `tensor_rope` (kernel/tensor.c): emparejaba `(i, i+head_dim/2)` (estilo rotate-half de HuggingFace) en vez de `(2i, 2i+1)` (estilo consecutivo de GGML/llama.cpp, el correcto para pesos GGUF de llama2.c/stories15M). El emparejamiento incorrecto degradaba la atención progresivamente con la posición, produciendo texto coherente al inicio y basura de subpalabras después.
- [x] `fixed_pow()` en `lib/math.c` era un stub sin implementar que siempre devolvía `1.0` sin importar base/exponente — colapsaba TODAS las frecuencias de RoPE a la misma. Reescritos `fixed_pow` (aproximación log2/pow2 estilo Ankerl) y `fixed_sin`/`fixed_cos` (reducción de rango a (-pi,pi] + aproximación de Bhaskara) usando floats de hardware.
- [x] **Causa raíz principal**: `tokenizer_encode()` (kernel/tokenizer.c) comparaba el prompt crudo del usuario (espacios ASCII normales) contra un vocabulario SentencePiece donde el espacio de inicio de palabra se codifica como ▁ (UTF-8 `E2 96 81`), no como `0x20`. Ningún token de inicio de palabra podía matchear nunca, así que el prompt completo le llegaba al modelo ya fragmentado en pedazos sin relación antes de que corriera el forward pass — la entrada estaba rota independientemente de la calidad de la matemática. Fix: preprocesar el texto reemplazando cada espacio (y el inicio del prompt) por el marcador ▁ antes del greedy match.
- [x] Penalty de repetición desproporcionado: `-40.0` flat sobre una ventana de 16 tokens prohibía por completo repetir CUALQUIER token reciente, incluyendo palabras función comunes ("a", "the", "to") que normalmente reaparecen en pocas frases — forzaba al decoder hacia continuaciones raras. Reducido a `-1.5` sobre una ventana real de los últimos 4 tokens (antes el índice de chequeo no correspondía a los tokens más recientes por el wraparound del ring buffer de 16 slots).
- [x] `fixed_exp()` usaba una serie de Taylor de 4 términos, válida solo para |x|≲2 — se usa en `tensor_softmax` (atención) y en `fixed_sigmoid`→SiLU (FFN), donde valores reales superan ese rango fácilmente (ej. x=-5 daba +13.7 en vez de 0.0067). Corrompía la distribución de atención y el gating de la FFN en todas las capas. Reescrito vía `exp2(x*log2(e))` reusando la aproximación rápida de `fixed_pow`.
- [x] Tras los fixes anteriores la generación sigue degenerando hacia los mismos fragmentos ("terin"/"aen"/"ter" recurrentes) — patrón sistemático, no ruido aleatorio. Agregado comando de diagnóstico `llm logits <prompt>` (syscall 59, `llm_dump_logits` en `kernel/llm.c`) que corre el prefill y vuelca al log del kernel la energía del hidden state (`h_sumsq`) y los top-5 logits/tokens de salida para el primer paso, en vez de generar texto.
- [x] **Hallazgo via `llm logits`**: `h_sumsq` se imprimía como `-2147483.-648` (≈ INT32_MIN) — overflow real, no solo un bug de formato. `fixed_exp()` (recién reescrito sin clamp) podía recibir argumentos grandes durante softmax/sigmoid en el forward pass real y producir un resultado cuya conversión a `fixed_t` (Q16.16, máx ~32767) desbordaba el `int32_t`; ese valor corrupto se propaga por las sumas residuales y matmuls de las capas siguientes, contaminando todo el cálculo sin importar el prompt — explica la convergencia degenerada a los mismos tokens. Fix: clamp de `fixed_exp` a [-10,10] antes de convertir a fixed_t (mismo rango que el código viejo, pero con la aproximación precisa adentro). También agregada guarda anti-NaN/Inf en el propio formateador del dump de diagnóstico.
- [x] El clamp de `fixed_exp` evitó el overflow de display, pero `h_sumsq` final sigue >2.000.000 (debería ser ~`n_embd`=288 tras `output_norm`) — la explosión real sigue sin resolverse, solo deja de corromper el número impreso. Agregada instrumentación por capa (`llm_debug_h_energy`, gateada por `llm_trace_on`) que loguea `h_sumsq` antes de las capas, después de cada sub-bloque de atención/FFN, y después de `output_norm`.
- [x] `llm_debug_h_energy` confirmó: la explosión ya está presente en `after-attn L0` (la primerísima capa) — `pre-layers` da ~0.2 (razonable) pero apenas termina la atención de la capa 0 ya está disparado. Bug encontrado de paso en la propia herramienta: el clamp de `append_float3` a ±1e9 seguía desbordando `int32_t` al multiplicar por 1000 para los decimales (1e9*1000=1e12 > INT32_MAX) — arreglado evitando esa multiplicación para valores >2.000.000 (se imprime solo la parte entera con un "+"). Agregados checkpoints más finos dentro del bloque de atención de la capa 0 (`L0 q post-proj`, `L0 q post-rope`, `L0 scratch post-vmix`, `L0 v post-oproj`).
- [x] **Causa raíz confirmada y resuelta**: `L0 q post-proj` salía roto (la proyección Q explotaba apenas se calculaba), mientras que `L0 scratch post-vmix` (que para el primer token equivale a copiar V) salía sano. `tensor_t` no tenía campo de tipo en absoluto — `llama_matmul_fpu` asumía Q4_0 para CUALQUIER tensor de pesos sin chequear el tipo real almacenado en el GGUF; `attn_q.weight` estaba en un tipo distinto a `attn_k`/`attn_v` en este modelo, así que se leían sus bytes como si fueran bloques Q4_0, produciendo basura desde la primera proyección y contaminando todo lo demás. Fix: agregado `gguf_type` a `tensor_t` (poblado en `tensor_load_gguf`/`tensor_load_gguf_view`), y `llama_matmul_fpu` ahora soporta Q4_0/F32/F16 según el tipo real. **Verificado**: tras el fix, `llm logits`/`llm ask` con stories15M ya no muestran overflow en ningún checkpoint de energía (valores sanos en todas las capas) y el texto generado tiene mayoría de palabras reales con mejor coherencia ("saved... only shared, and... okay other fun... so wrong next like remember game light") en vez de fragmentos sin sentido.

#### Etapa 10: Investigación de coherencia en modelos grandes (TinyLlama-1.1B-Chat)

Motivado por probar `loadmodel`/`-initrd` con TinyLlama real por primera vez (nunca antes booteado en esta maquina, ver nota de `-mstackrealign` en el Roadmap de RAM Completa): el texto generado no armaba oraciones con sentido, a diferencia de stories15M. Investigacion sobre `kernel/tokenizer.c` y `kernel/llm.c` mas metadata real del GGUF (via script python standalone, no el kernel) encontro 3 problemas concretos:

- [x] **Nunca se anteponia el token BOS** — el archivo declara `tokenizer.ggml.bos_token_id=1` pero ni `tokenizer_encode()` ni la generacion lo usaban; el prompt se codificaba sin el token con el que todo modelo Llama fue entrenado a empezar cada secuencia. Fix: `llm_get_bos_token_id()` (lee la metadata real, fallback a 1) y un paso extra de forward-pass en posicion 0 para BOS antes del prompt real, en `llm_try_gguf_inference` y `llm_dump_top_logits` (`kernel/llm.c`). Verificado por conteo de forward-passes en QEMU (17 pasadas = 1 BOS + 14 prompt + 2 generados, matchea exacto).
- [x] **El token EOS nunca se chequeaba durante la generacion** — el loop corria hasta `max_new_tokens`/timeout sin importar si el propio modelo prefería terminar, forzando continuacion despues del punto donde un decoder correcto pararia. Fix: `llm_get_eos_token_id()` + `llm_output_logit_for_token()` (reusa los mismos paths rapidos Q4_0/Q6_K del argmax) comparando el logit de EOS contra el mejor candidato imprimible en cada paso; si EOS gana, corta la generacion en vez de ignorarlo.
- [x] **El tokenizer no hacia BPE real** — usaba un heuristico "greedy: substring completo mas largo que matchee en el vocabulario", ignorando por completo `tokenizer.ggml.scores` (`t->scores` quedaba `NULL`, nunca cargado). Reescrito `kernel/tokenizer.c`: ahora carga `scores` desde GGUF, cachea longitudes de vocab (`t->lengths`, evita recalcular `strlen` en cada intento de merge), y `tokenizer_encode()` implementa el algoritmo real de SentencePiece/Llama BPE (split en simbolos UTF-8 de a uno, merge repetido del par adyacente con mayor score cuya concatenacion exista en el vocabulario, hasta que no queden merges posibles) en vez de la aproximacion greedy. Simbolos finales que no matchean ningun token completo caen a tokens `<0xXX>` de byte-fallback en vez de descartarse en silencio. **Verificado**: `stories15M` sigue pasando `llm selftest gguf` sin regresion (mismo texto de antes, con mejor estructura de clausulas tras el fix de BOS/EOS: "the sun was a little girl, there were two friends, even though..."); para "hola" con TinyLlama el resultado coincide con el heuristico anterior (`▁hol`+`a`) — coincidencia esperable ya que ambos algoritmos convergen quando el candidato mas largo tambien es el de mayor score, no evidencia de que el fix este de mas.
- [x] **Coherencia de TinyLlama-1.1B-Chat evaluada a fondo: sigue rota en generacion larga, mas alla de los 2-3 tokens antes evaluados** - Descargado `tinyllama-1.1b-chat-v1.0.Q4_0.gguf` (TheBloke, ~608MB) y corrido `llm chat tell me about the sun` en QEMU (`-initrd`, sin high memory, `-m 1024M` para no activar el bug sin resolver de PAE/migracion de tensores documentado en "Roadmap de RAM Completa"). Con el timeout de 30s de `LLM_GGUF_PREVIEW_TIMEOUT_TICKS` (pensado para detectar cuelgues, no para acotar generacion real) el prefill del prompt con template de chat ni siquiera completaba un token en este host (WSL2 sin KVM/aceleracion por hardware, TCG puro: ~10s por pasada completa de 22 capas, mucho mas lento que el host donde se midieron los ~15-20s/token previos) — subido temporalmente a 600s solo para este diagnostico (revertido antes de commitear, sin cambios netos en `kernel/llm.c`). Con eso, la generacion corrio 11 tokens reales antes de cortarse por limite de tiempo del test: `ston`, ` sorry`, ` OS`, ` share`, ` gegen`, `signed`, `teil`, ` undefined`, ` Star`, ` padding`, ` Message` — ensalada de palabras sin relacion sintactica ni semantica entre si (incluye fragmentos en aleman y tokens tipo `OS`/`undefined`/`Message` ajenos a cualquier respuesta plausible sobre "the sun"), claramente peor que la coherencia parcial ya documentada para `stories15M` con el mismo backend. Los checkpoints de energia (`LLM ENERGY ...`) se mantienen en rangos sanos capa a capa sin overflow ni explosion (`post-output_norm h_sumsq` estable alrededor de 7500-7600 en todos los pasos), descartando que sea el mismo tipo de bug numerico ya resuelto en la Etapa 10 de `stories15M` — la causa de la falta de coherencia en TinyLlama sigue sin diagnosticarse (candidatos no investigados todavia: cuantizacion Q4_0 en un modelo con n_embd/n_head mucho mayor podria acumular mas error por bloque, algun detalle de GQA/RoPE especifico de esta arquitectura, o el propio template de chat/tokenizacion BPE con vocabulario mucho mas grande que el de stories15M). Queda como bug abierto documentado, no como "pendiente de evaluar".

#### Etapa 10: Criterio de "LLM real" — ALCANZADO

Se considera alcanzado cuando:

- [x] Un GGUF tiny responde a prompts no hardcodeados — `llm_try_gguf_inference` ejecuta forward pass real con argmax greedy.
- [x] La respuesta se genera token por token — bucle autoregresivo en `llm.c`; hasta 48 tokens con KV cache incremental.
- [x] El codigo ejecuta embeddings, attention, FFN, normalizacion y sampler — `run_llama_forward_pass`: RMSNorm, Q/K/V matmul, RoPE, softmax, V mix, O projection, SwiGLU FFN, residual, output norm, logits argmax.
- [x] `llm ask` no depende de pares de reglas ni features MKNN — cuando `LLM_BACKEND_GGUF_GEN` activo, `llm_inference` delega a `llm_try_gguf_inference`.
- [x] Existe una prueba reproducible en QEMU — `make qemu-gguf` + `loadmodel model.gguf` + `llm ask hola` + `llm selftest gguf`.

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
- [x] **FAT32 VFAT Unicode + checksum LFN** - Experimental; nombres largos ahora se decodifican a UTF-8 real (BMP completo, vía `fat32_lfn_to_utf8`) en vez de reemplazar todo carácter no-ASCII por '?'. Se valida el checksum estándar MS-DOS de cada secuencia LFN contra el nombre corto 8.3 asociado (`fat32_lfn_checksum`/`fat32_lfn_validate`) — una secuencia LFN huérfana o corrupta (ej. tras una escritura parcial) ya no se muestra como si perteneciera al archivo equivocado, cae al nombre corto. Refactorizada la lógica (antes triplicada con un bug latente potencial en cada copia) en un `fat32_lfn_state_t` + `fat32_lfn_feed`/`fat32_lfn_reset` compartido por los 3 sitios que recorren directorios (`fat32_find_entry_in_dir`, `fat32_ls_path`, `fat32_get_entry_by_index`). Limitación conocida: codepoints fuera del BMP (pares sustitutos) no se combinan, caso extremadamente raro en nombres de archivo reales.
- [x] Verificado host-side con `scripts/fat32_lfn_verify.c` (reimplementa el mismo algoritmo, standalone, contra los bytes reales de un `storage.img` armado con `mtools`): un archivo `café del día muy largo y con ñ.txt` se decodifica perfecto en UTF-8 y el checksum LFN valida `YES` contra todos los archivos reales.
- [x] **Verificado end-to-end en QEMU real** (`scripts/test_vfat_unicode.sh` arma el fixture; boot manual con `qemu-system-i386 -kernel build/kernel.bin -initrd stories15M.gguf -drive file=storage.img,format=raw,index=0,media=disk ...` para no pisar el fixture con el target `image-gguf` del Makefile): `ls` muestra `café del día muy largo y con ñ.txt` con tildes/ñ correctos, y `cat caf<TAB>` autocompleta el nombre Unicode y lee el contenido del archivo correctamente — confirma tanto el listado de directorio como la resolución de ruta (`fat32_find_entry_in_dir`) con LFN Unicode real.
- [x] **ext2 read-only real: validación amplia** - Experimental; `kernel/extfs.c` confiaba en campos del disco sin validar: (1) un `name_len` de entrada de directorio corrupto/malicioso podía exceder lo que `rec_len` reservaba, causando una lectura fuera de los límites del buffer de bloque al copiar/comparar el nombre — agregado `ext_dir_entry_valid()` (valida `rec_len`/`name_len` contra el tamaño del bloque) en los dos loops que recorren directorios (`ext_find_entry_in_dir`, `extfs_ls`); (2) números de bloque corruptos en punteros indirectos podían desbordar `block*block_size` (overflow de `uint32_t`) y terminar leyendo otro offset válido pero incorrecto en silencio — `ext_read_block` ahora rechaza bloques `>= blocks_count`; (3) un `inode_num` corrupto podía producir un índice de grupo arbitrario — `ext_read_group_desc` ahora valida contra la cantidad real de grupos; (4) `ext_mount` ahora rechaza superbloques con `blocks_per_group`/`inodes_per_group`/`blocks_count`/`inodes_count` en cero o `inode_size` fuera de rango sano, en vez de fallar más tarde con una división por cero o una lectura fuera de rango en un lugar menos obvio.
- [x] **USB Mass Storage — Etapa 0 a 4 completas** (deteccion/init de UHCI, enumeracion, Bulk-Only Transport + SCSI minimo, integracion como `block_device_t`) - Experimental; ver "Roadmap USB Mass Storage" mas abajo para detalle y verificacion. Un pendrive USB en QEMU ya se detecta, enumera, habla SCSI y se monta como FAT32 de solo lectura junto al disco ATA — sin cambios en FAT32/ext2/VFS.

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
- [x] **Framebuffer gráfico real** - Experimental; ya existía soporte real (no solo lo del Roadmap NVIDIA Etapa 1): plot de píxeles 32/24/16bpp, render de caracteres con fuente bitmap 8x8, fallback a VGA texto. Faltaba scroll real — al llenar la última fila de caracteres, `video_putc` borraba TODO el framebuffer y reseteaba el cursor a (0,0) en vez de desplazar el contenido hacia arriba como una terminal real. Agregado `fb_scroll_locked()` (desplaza el framebuffer un carácter-fila vía `memmove`, limpia solo la fila nueva expuesta) y `memmove` real a `lib/string.c` (antes solo había `memcpy`, que es UB para rangos superpuestos aunque la implementación simple "funcionara" por casualidad para esta dirección específica).
- [x] Corrección de exactitud: el ítem "Mapear framebuffer como memoria write-combining" (Roadmap NVIDIA Etapa 1) estaba marcado [x] pero es inexacto — `vmm.c` solo mapea el framebuffer con flags `0x07` (Present|RW|User), sin configurar PAT/PCD para WC real. Sin efecto práctico en QEMU (TCG no modela coherencia de caché), pero la entrada no reflejaba el código real. PAT/WC real queda pendiente si se necesita en hardware real.
- [ ] **Driver GPU compute** - Pendiente.
- [x] **Network stack** - Experimental; e1000 driver, ARP, IPv4, ICMP, UDP dispatch, DHCP, DNS, LLM UDP service, RSH.

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
- [x] Separar stats de pools PMM low/high.
- [x] Implementar asignacion real desde pool alto para paginas temporales.
- [x] Hacer el pool alto consciente de regiones reales del mmap, sin asumir RAM contigua arriba de 4GB.
- [x] Implementar estructuras PAE y helpers de entradas de 64 bits.
- [x] Implementar ventanas temporales reales de high memory sobre PAE.
- [x] Soportar memoria alta con PAE para RAM >4GB como memoria general - antes solo existia `vmm_temp_map_high()` con una unica ventana fija de 4KB (0xFF000000), suficiente para el test de verificacion puntual pero no para uso general (no se podia tener mas de una pagina alta mapeada a la vez, ni pedir un buffer logico mas grande que 4KB). Agregado `vmm_temp_map_high_slot(slot, phys)`/`vmm_temp_unmap_high_slot(slot)` en `kernel/vmm.c` (4 ventanas de 4KB en 0xFF000000-0xFF003FFF; `vmm_temp_map_high`/`vmm_temp_unmap_high` originales ahora son slot 0, sin cambio de comportamiento) y un modulo nuevo `kernel/highmem.c`/`highmem.h` con `hbuf_t`: un buffer logicamente contiguo respaldado por N paginas fisicas de alta memoria no necesariamente contiguas (`hbuf_alloc`/`hbuf_free`/`hbuf_read`/`hbuf_write`), copiando de a una pagina por vez a traves de una ventana temporal dedicada (slot 1). `hbuf_selftest()` (syscall 60, comando `highmemtest buf`) verifica alloc/write/read de 3 paginas, un acceso sin alinear que cruza el limite de pagina, y que un acceso fuera de rango falle en vez de truncar en silencio; devuelve SKIP (no FAIL) si no hay RAM >4GB en este boot, seven la convencion PASS/FAIL/SKIP de `llm selftest gguf`. **Verificado en QEMU real** (`qemu-system-i386 -m 6G`, `make test-netcfg`-style boot manual con `-nic none` para no pisar el bug de e1000 documentado abajo): `mem` reporta el pool alto (~272877/1048575 bloques), `highmemtest` (ventana unica, preexistente) y `highmemtest buf` (multi-pagina, nuevo) devuelven PASS.
- [x] **Bug de compilador encontrado al probar lo anterior**: `hbuf_selftest()` crasheaba con general protection fault (vector 0x0D) en la primerisima corrida real en QEMU. Diagnostico: agregado impresion de EIP/CS al panic de `fault_handler` (`kernel/idt.c`, antes solo mostraba registros generales) para poder ubicar la instruccion exacta con `objdump -d`; resulto ser `movdqa %xmm0,(%eax)` con EAX no alineado a 16 bytes — GCC con `-O3 -msse2` autovectoriza el loop que llena `pattern[]` en `hbuf_selftest` asumiendo que la pila esta alineada a 16 bytes por la ABI SysV i386, pero el codigo de entrada a syscalls/interrupciones escrito a mano (`kernel/interrupts.asm`) no garantiza esa alineacion al llamar a C. Mismo genero de bug que el fix de FXSAVE documentado en Fase 1 (SSE + contexto de interrupcion), pero a nivel compilador en vez de guardado de registros. Fix: agregado `-mstackrealign` a `CFLAGS` (Makefile) — instruye a GCC a realinear la pila en el prologo de cualquier funcion que lo necesite en vez de confiar en la alineacion de quien llama. Protege contra esta clase de bug en **todo** el kernel, no solo en `highmem.c`; probablemente estaba latente en otro codigo autovectorizado (`tensor.c`, `llm.c`) sin haberse disparado aun por casualidad de layout de pila.
- [x] **Bug de red descubierto durante esta verificacion (no relacionado a high memory, documentado para no perderlo)**: cualquier boot con NIC presente (el default de QEMU si no se pasa `-nic none`) crasheaba con general protection fault (vector 0x0D) justo despues de `"Network: Intel e1000 NIC initialized."`, dentro de `e1000_init_rings()` (`kernel/net.c`). Era la primera vez que este build (con meses de cambios sin commitear en `net.c`, `llm.c`, etc. — nunca antes compilado/booteado en esta maquina por falta de toolchain) corria en QEMU real; el roadmap de red de mas arriba describe estas features como verificadas, pero esa verificacion habria sido en otra maquina/sesion previa al estado actual del codigo. **Resuelto por el mismo fix de arriba**: confirmado en QEMU que con `-mstackrealign` el boot con NIC por defecto llega limpio hasta `"Network: e1000 RX/TX rings initialized."` y al shell — era exactamente el mismo bug de alineacion SSE/stack, no algo especifico de red.
- [x] **Mover tensores de pesos GGUF a high memory usando `hbuf_t` (lectura funciona; liberar la memoria baja original queda pendiente por un bug sin resolver)**. Agregado `gguf_migrate_data_to_high_mem`/`gguf_release_high_mem_backing`/`gguf_read_data_bytes` en `kernel/llm_gguf.c` (cache de un solo slot, igual patron que el cache de `gguf_probe`): tras cargar un modelo GGUF valido de al menos 8MB de seccion de datos, si hay pool alto disponible, copia `[data_offset, file_size)` a un `hbuf_t` y todo lectura de bytes de tensores (`llama_matmul_fpu`, el scan de argmax de vocabulario en `llm_try_gguf_inference`/`llm_dump_top_logits`, `tensor_load_gguf`, `tensor_read_gguf_row_into`) pasa por este accessor en vez de puntero directo — con buffers de scratch persistentes (`llm_row_scratch_ensure`/`tensor_row_scratch_ensure`) para no hacer kmalloc/kfree por fila (un intento inicial sin esto volvio `llm selftest gguf` con stories15M ~10x mas lento). Sin high memory disponible (el caso comun), el comportamiento es identico al anterior (mismo texto generado, mismos tokens, verificado). El metadata/vocab/header del archivo NUNCA se migra, sigue siendo lectura directa de memoria baja como siempre.
- [x] **Bug encontrado y sin resolver: liberar la memoria baja original tras migrar corrompe datos de forma rara y reproducible**. El diseno original tambien liberaba `[data_offset, file_size)` de vuelta al pool bajo via `pmm_free_region()` tras copiar. Verificacion por checksum de 32 filas muestreadas del tensor `output.weight` (Q6_K, TinyLlama) con `-m 6G` mostro **una sola fila** (de 32) con checksum distinto entre memoria baja y memoria alta, reproducible byte-a-byte en corridas separadas. Investigacion exhaustiva descarto: tamano de chunk de copia (probado 64KB y 4KB, misma falla exacta), interrupciones del timer (falla igual con `cli`/`sti` alrededor del loop de copia), paginas fisicas duplicadas dentro del mismo `hbuf_t` (escaneado el array completo, sin duplicados), el mecanismo de lectura/escritura en si (un patron de prueba propio hace roundtrip perfecto en la misma direccion fisica), inestabilidad de la fuente en memoria baja (checksum identico antes/despues de toda la copia), y lectura "flaky" (releer da el mismo valor incorrecto de forma estable). Instrumentacion directa dentro de `hbuf_copy` (`kernel/highmem.c`, logueando cada toque de la pagina afectada con su direccion de retorno) confirmo que `vmm_temp_map_high_slot` (usado exclusivamente desde `hbuf_copy`, el UNICO mecanismo en todo el kernel para tocar fisica >4GB) solo toca esa pagina 3 veces — la escritura original (verificada correcta al instante, en la misma iteracion del loop) y dos lecturas propias de diagnostico — nada mas. Un watchpoint de hardware de GDB sobre la direccion fisica exacta se descarto como enfoque porque enfrentaria el mismo cuello de botella que un breakpoint condicional (~148000 escrituras legitimas pasan por el unico VA compartido, cada una requeriria evaluacion via protocolo remoto). `info mtree` del monitor de QEMU confirmo que la RAM arriba de 4GB es un alias interno de QEMU sobre el mismo backing que la RAM baja con un offset de host distinto — normal, no parece la causa. **Sin causa raiz identificada**; posible bug de emulacion TCG en direccionamiento fisico >4GB via PAE, indetectable sin herramientas de trazado interno de QEMU. **Mitigacion aplicada**: `gguf_migrate_data_to_high_mem` ya NO libera la memoria baja original (el `pmm_free_region()` fue removido) — los tensores se leen correctamente desde el `hbuf_t` en memoria alta (verificado exhaustivamente), pero los bytes originales en memoria baja quedan reservados sin usar en vez de liberarse. Esto es seguro (no hay bug de correctitud en lo que queda activo) pero no logra el objetivo de liberar RAM baja que era la motivacion original de este item.

### Roadmap USB Mass Storage

Objetivo: poder montar un pendrive USB con las mismas capas FAT32/ext2/VFS que ya
existen, sin cambios en el filesystem layer — igual que `ata_primary_master()` se
enchufa hoy via `block_device_t`. El stack completo (driver de controlador +
enumeracion de dispositivo + Bulk-Only Transport + comandos SCSI +
`block_device_t`) es comparable en tamano a todo lo que ya existe de ATA y red
juntos, y es codigo nuevo, pesado en protocolo, sin precedente en este codebase —
riesgo real de bugs sutiles de timing/protocolo dificiles de debuggear (como el de
la migracion a high memory documentado arriba). Por eso se aborda en etapas chicas
y verificables independientemente, igual que ATA/FAT32/ext2/red/NVIDIA.

#### Etapa 0+1: Deteccion e inicializacion del controlador UHCI

- [x] **Deteccion de controlador UHCI via PCI** - Experimental; `kernel/uhci.c` escanea `pci_device_count()`/`pci_get_device()` buscando `class_code=0x0C` (Serial Bus Controller), `subclass=0x03` (USB), `prog_if=0x00` (UHCI) — el unico controlador USB puramente basado en I/O ports (sin MMIO) que expone QEMU por defecto en el chipset i440fx/PIIX3 (`-usb`, sin necesitar mapeo de BARs nuevo que este kernel no tiene para OHCI/EHCI/xHCI). I/O base viene de `bar[4] & 0xFFFC`.
- [x] **Bring-up del controlador** - Experimental; reset global (`USBCMD.GRESET`), allocacion de un frame list de 1024 entradas de 32 bits alineado a 4KB via `pmm_alloc_block()` (todas las entradas en terminate — sin queue heads programadas todavia, eso es Etapa 2), `FRBASEADD`/`FRNUM` programados, `USBINTR=0` (solo polling, sin IRQs), y arranque con `USBCMD = RUN|CF|MAXP64`.
- [x] **Reset y deteccion de los 2 puertos** - Experimental; para cada puerto: lee `PORTSC`, si `CCS` esta seteado asume reset (`PR` por >=50ms, clear, >=10ms de recovery), despues `PE` y confirma enable + reconexion; guarda low-speed vs full-speed via `LSDA`.
- [x] **Comando `usb` de shell + syscall 61** - Experimental; `uhci_status_string()` expuesto igual que `gpu`/`gpu info`, ademas de logearse automaticamente en el boot (`storage_init()`) sin necesitar el comando.
- [x] **Bug encontrado y corregido durante la verificacion: el bring-up colgaba el boot entero con un dispositivo USB real conectado**. El delay entre pasos del reset de puerto usaba `timer_get_ticks()` (`uhci_delay_ticks`, por analogia con el resto del kernel), pero `storage_init()` corre **antes** de que el scheduler haga el primer cambio de tarea — las interrupciones recien se habilitan cuando se ejecuta el primer `iret` a Ring 3 con `EFLAGS=0x202` (`kernel/task.c`). Sin el timer avanzando, el busy-wait basado en ticks nunca termina. Sin un dispositivo USB conectado el bug era invisible (`uhci_find_controller` fallaba antes de llegar a ningun delay), lo cual permitio que compilara y bootee "bien" hasta que se probo con `-usb -device usb-storage`. Mismo tipo de trampa que `kernel/ata.c` ya evita: reemplazado por `uhci_io_delay()`, un busy-wait puro de puertos I/O (`inb(0x80)`, el truco clasico de "puerto de diagnostico" para delays sin depender de interrupciones), igual filosofia que `ata_io_wait`.
- [x] **Verificado en QEMU real** (`make qemu-usb`, agrega `-usb -drive if=none,format=raw,file=usbstick.img,id=usbstick -device usb-storage,drive=usbstick` sobre el target `qemu` existente): (1) sin `-usb`, el boot llega limpio hasta el shell y `usb`/el log de boot reportan "no UHCI controller found" — sin regresion; (2) con `-usb -device usb-storage`, el log de boot muestra `USB: UHCI controller at I/O base 0xC040` y `port 0: device connected, enabled, full-speed`, y el resto del boot (storage, red, carga de LLM, shell) continua sin verse afectado.

#### Etapa 2: Enumeracion de dispositivo (DONE)

- [x] **Motor de transferencias de control (Queue Head + Transfer Descriptors)** - Experimental; una sola QH estatica (`uhci.ctrl->qh`) enlazada en las 1024 entradas del frame list (Etapa 0+1 las dejaba todas en terminate), mas un pool de hasta 32 TDs y un buffer de 256 bytes, todo en una unica pagina de `pmm_alloc_block()`. `uhci_control_transfer()` arma Setup/Data/Status como submits separados contra la misma QH (evita depender de la semantica de "corte automatico del HC ante short packet" dentro de una cadena larga) y `uhci_run_phase()` sondea cada TD hasta que `Active` se limpia, con `SPD` habilitado en las fases IN para que pedir mas bytes que los que un descriptor realmente tiene no cuente como error.
- [x] **`SET_ADDRESS`/`GET_DESCRIPTOR`/`SET_CONFIGURATION`** - Experimental; `uhci_enumerate_port()` lee 8 bytes del device descriptor con max-packet=8 (minimo universal del endpoint 0 por spec) para aprender el `bMaxPacketSize0` real, asigna direccion `port+1` (sin soporte de hubs), relee el device descriptor completo, pide el config descriptor (primero el header de 9 bytes para `wTotalLength`, despues completo) y hace `SET_CONFIGURATION`. `uhci_parse_config_descriptor()` extrae clase/subclase/protocolo de interfaz y los endpoints bulk IN/OUT (direccion + `wMaxPacketSize`) para dejarlos listos para Bulk-Only Transport (Etapa 3).
- [x] **Bug encontrado y corregido: timeout de TD basado en iteraciones de CPU, no en tiempo real** - El primer intento media el timeout de cada TD con un contador fijo de iteraciones de `inb(0x80)` (mismo orden de magnitud que los delays de reset de puerto de Etapa 0+1). Bajo TCG a velocidad nativa esas iteraciones terminan en muchisimo menos de 1ms real, mientras que el contador de frames del propio controlador (`FRNUM`) solo avanza con el timer interno de 1ms de QEMU — el resultado era un timeout instantaneo con el TD ni siquiera tocado por el HC. Fix: `uhci_run_phase()`/`uhci_delay_frames()` ahora sondean `FRNUM` directamente (`uhci_frnum()`) para medir tiempo real transcurrido en vez de contar iteraciones de CPU.
- [x] **Bug encontrado y corregido: falta `Bus Master Enable` en el registro de comando PCI del controlador** - El bring-up de Etapa 0+1 nunca tocaba el PCI Command register de la UHCI; los accesos a puertos I/O (PORTSC, USBCMD, etc.) funcionaban igual porque son I/O directo, pero las lecturas/escrituras DMA del propio HC sobre el frame list/QH/TD (que es como real y unicamente puede tocar la RAM del sistema) dependen del bit Bus Master Enable (PCI Command, bit 2). Sin el, era indistinguible de "el HC nunca procesa nada" desde el lado del kernel. Fix: `uhci_find_controller()` ahora hace `command |= 0x0005` (I/O Space Enable + Bus Master Enable) sobre el PCI Command register al encontrar el controlador, mismo patron que `kernel/net.c` ya usa para el BAR MMIO del e1000.
- [x] **Bug encontrado y corregido (causa raiz real, no el de Bus Master de arriba): estructuras QH/TD sin `volatile`** - Incluso con Bus Master Enable corregido, el primer TD seguia sin completarse nunca — `FRNUM` avanzaba con normalidad (confirmando que el HC estaba corriendo) pero `td->status` se veia congelado en el valor inicial indefinidamente. Instrumentacion con `-trace 'usb_uhci_*'` de QEMU confirmo que la QH SI se cargaba cada frame (miles de `usb_uhci_qh_load`), pero nunca tomaba la rama "con elementos" — es decir, el HC nunca veia el `qh.element` que el kernel escribia. Causa real: `uhci_qh_t`/`uhci_td_t` declaraban sus campos como `uint32_t` planos; sin `volatile`, GCC (`-O3`) no tiene forma de saber que esa memoria cambia por DMA externo del hardware y puede cachear/no-recargar una lectura que a simple vista nunca se modifica en el codigo visible — el loop de polling podia quedarse leyendo un valor de registro obsoleto para siempre. `FRNUM` nunca mostro el sintoma porque se lee via `inw()` (`asm volatile`), lo cual desvio la sospecha inicial hacia un problema de DMA/bus-mastering en vez de aliasing del compilador. Fix: los 6 campos de `uhci_qh_t`/`uhci_td_t` ahora son `volatile uint32_t`.
- [x] **Verificado en QEMU real** (`make qemu-usb`): con `-usb -device usb-storage,drive=usbstick`, el log de boot ahora muestra enumeracion completa — `addr 1, VID:PID 0x46F4:0x0001, class 0x00/0x00/0x00`, `configured, interface class 0x08/0x06/0x50 (mass storage)` (0x08=Mass Storage, 0x06=SCSI transparente, 0x50=Bulk-Only Transport, exactamente lo esperado de `usb-storage` de QEMU) y `bulk IN ep1 (mps 0040), bulk OUT ep2 (mps 0040)`. Sin `-usb`, el boot sigue reportando "no UHCI controller found" sin regresion, y el resto del boot (storage, red, LLM, shell) no se ve afectado en ningun caso.

#### Etapa 3: Bulk-Only Transport + SCSI minimo (DONE)

- [x] **Transferencias bulk sobre UHCI** - Experimental; `uhci_bulk_transfer()` reutiliza la misma QH/pool de TDs de las transferencias de control (Etapa 2) — este driver nunca tiene control y bulk en vuelo a la vez, así que una segunda QH/entrada de frame list habría sido complejidad sin uso. El pool de TDs se subió de 32 a 64 (`UHCI_CTRL_MAX_TD`) para cubrir hasta 4KB (8 sectores de 512 bytes al max-packet bulk full-speed de 64 bytes) en una sola cadena. El toggle de datos (DATA0/DATA1) es persistente por pipe (`dev->bulk_in_toggle`/`bulk_out_toggle`, reseteado a 0 en `SET_CONFIGURATION` como exige el spec) y se ajusta correctamente incluso si la cadena termina antes de tiempo (`uhci_run_phase` ahora devuelve cuántos TDs completaron realmente vía `out_done`, no solo cuántos se pidieron).
- [x] **CBW/CSW (Bulk-Only Transport)** - Experimental; `uhci_msd_command()` arma el Command Block Wrapper de 31 bytes (firma `USBC`, tag incremental, `dCBWDataTransferLength`, flags de dirección, CDB de hasta 16 bytes), hace la fase de datos opcional por el pipe bulk correspondiente, y valida el Command Status Wrapper de 13 bytes (firma `USBS`, tag que debe coincidir, `bCSWStatus`).
- [x] **`TEST UNIT READY`, `INQUIRY`, `READ CAPACITY(10)`, `READ(10)`, `WRITE(10)`** - Experimental; wrappers finos sobre `uhci_msd_command()` que arman el CDB de 6/10 bytes correspondiente. `READ(10)`/`WRITE(10)` heredan el límite de una sola cadena de `uhci_bulk_transfer` (hasta 8 sectores de 512 bytes por llamada) — suficiente para esta etapa de verificación, la integración real como `block_device_t` (Etapa 4) decidirá si hace falta trocear transferencias más grandes en múltiples llamadas.
- [x] **Comando de shell `usb msdtest`** (solo lectura: INQUIRY + TEST UNIT READY + READ CAPACITY(10) + READ(10) LBA 0) **y `usb msdwritetest`** (agrega un roundtrip WRITE(10)+READ(10) en un LBA de scratch, 100) - Experimental; syscall 62 (`SYS_USB_MSD_TEST_STR`), mismo patrón que la syscall 61 de `usb`. El test de escritura queda deliberadamente separado del de solo-lectura y no se ejecuta nunca automáticamente — a diferencia de la enumeración (Etapa 2), que es puramente diagnóstica, `WRITE(10)` modifica el medio de verdad, y correrlo sin que el usuario lo pida explícitamente sería riesgoso si algún dia se prueba contra un pendrive real en vez de `usbstick.img`.
- [x] **Diagnóstico de solo lectura automático en el boot** - `storage_init()` ahora también loguea `uhci_msd_test_string(0)` (solo lectura) justo después del status de enumeración, mismo criterio que ya se usaba para no exigir el comando `usb` para ver si hay controlador — sin dispositivo de almacenamiento presente, imprime una sola línea y sigue sin afectar el resto del boot.
- [x] **Bug encontrado y corregido de paso (no de esta etapa): el `usb` interactivo truncaba la salida a 255 bytes** - `uhci_status_buf` se había ampliado a 512 bytes al agregar el detalle de dispositivo enumerado en Etapa 2, pero la syscall 61 y el buffer del lado shell seguían fijos en 256 — invisible en el log de boot (que llama a `klog(uhci_status_string())` directo, sin pasar por la syscall) pero el comando interactivo `usb` sí lo hubiera mostrado cortado para un dispositivo mass-storage real. Buffers y `uptr_ok`/`strncpy` de la syscall subidos a 512 en `kernel/shell.c`/`kernel/syscall.c`.
- [x] **Verificado en QEMU real** (`make qemu-usb`): el log de boot ahora muestra, inmediatamente después del status de enumeración — `USB MSD: INQUIRY ok, vendor='QEMU' product='QEMU HARDDISK'`, `USB MSD: TEST UNIT READY ready`, `USB MSD: capacity 0x00020000 blocks x 0x00000200 bytes` (65536 sectores × 512 bytes = 64MB, coincide exacto con `usbstick.img`), y `USB MSD: READ(10) LBA 0 ok, first 16 bytes: EB 58 90 6D 6B 66 73 2E 66 61 74 00 02 01 20 00` — el string ASCII `mkfs.fat` visible en el OEM name del boot sector confirma que son bytes reales leídos del disco, no basura. Habilitando temporalmente el test de escritura se confirmó ademas `USB MSD: WRITE(10)+READ(10) roundtrip verified at LBA 0x64`. Sin `-usb`, el boot no se ve afectado (mismo comportamiento que Etapa 2).

#### Etapa 4: Integracion como `block_device_t` (DONE)

- [x] **`uhci_msd_block_read`/`uhci_msd_block_write` + `uhci_msd_init`/`uhci_msd_primary`** - Experimental; misma forma exacta que `ata_block_read`/`ata_block_write`/`ata_primary_master` en `kernel/ata.c` — offset/size en bytes, traducidos a LBA + sector-offset internamente, un sector SCSI por llamada (no se usa la capacidad de `uhci_bulk_transfer` de encadenar hasta 8 sectores en una sola transferencia BOT; esta etapa pedía correctitud, no rendimiento, y así se reutiliza tal cual el manejo ya probado de escritura parcial read-modify-write de `ata_block_write`). `uhci_msd_init()` corre `READ CAPACITY(10)` una vez para poblar `block_size`/`block_count` del `block_device_t` (capado a 4KB de tamaño de bloque como guarda defensiva) y guarda el `uhci_device_t*` real en `driver_data` para que los callbacks de lectura/escritura sepan a qué dirección/endpoints/toggle hablarle.
- [x] **Registro en `storage_init()`** - `kernel/storage.c` ahora llama `uhci_msd_init()` + `blockdev_register()` + `partition_scan_mbr()` inmediatamente después del diagnóstico de Etapa 3, mismo patrón que el bloque de ATA que ya existía — sin ningún cambio en `fat32.c`/`extfs.c`/`partition.c`, que no tienen idea de que el `block_device_t` que reciben es USB en vez de ATA.
- [x] **Verificado en QEMU real** (`make qemu-usb`): el log de boot ahora muestra, ademas del diagnóstico SCSI de Etapa 3, `Storage: USB mass-storage device detected.` y `Storage: no MBR/GPT partitions found on USB device.` (el `usbstick.img` de prueba está formateado directo con `mkfs.fat`, sin tabla de particiones, igual que `storage.img`); el loop generico de montaje de `storage_init()` (`for i in blockdev_count(): vfs_mount(...)`) imprime `Storage: filesystem mounted read-only. / fat32` **dos veces** — una por cada `block_device_t` registrado (ATA y USB) — confirmando que `fat32_probe`/`fat32_mount` reconocen el dispositivo USB sin cambios de código. Sin `-usb`, el boot es identico a antes (una sola vez, solo ATA).
- [x] **Limitación conocida, preexistente, no introducida por esta etapa**: `kernel/fat32.c` guarda el estado de montaje FAT32 activo en un único puntero global (`global_fat32_mount`), no una tabla por dispositivo — cuando hay dos `block_device_t` con FAT32 montados (ATA + USB), el último `vfs_mount()` de la lista "gana" y es el que usan `ls`/`cat`/`loadmodel`. `storage_init()` registra/monta USB *antes* que ATA a propósito, así que ATA (el disco de la demo, con `model.mklm`) sigue siendo el activo — pero esto depende del orden de llamadas, no es un diseño multi-mount real. Habilitar acceso simultáneo a ambos dispositivos vía shell (ej. `ls usb:/`) sería trabajo nuevo de VFS, fuera del alcance de "cero cambios en FAT32/ext2" que pedía esta etapa.

#### Criterio de listo — ALCANZADO

- [x] `make qemu-usb` levanta MicroK con un controlador UHCI y un dispositivo `usb-storage` visibles.
- [x] `usb` (o el log de boot) muestra puerto conectado/habilitado, VID:PID, clase de interfaz y endpoints bulk.
- [x] `usb msdtest` (tambien automatico en el boot, solo lectura) corre INQUIRY/TEST UNIT READY/READ CAPACITY(10)/READ(10) contra el dispositivo real.
- [x] `usb msdwritetest` prueba WRITE(10) con un roundtrip verificado en un LBA de scratch.
- [x] El dispositivo USB aparece como `block_device_t` y `partition_scan_mbr`/`vfs_mount` lo reconocen (FAT32) sin cambios en el filesystem layer.
- [x] Sin `-usb`, el boot y el resto del stack de storage/red/LLM no se ven afectados en ningun punto de las 4 etapas.
- [ ] Enumeracion de mas de un dispositivo simultaneo y acceso concurrente a ambos mounts FAT32 vía shell — no pedido por el roadmap original, ver limitación de `global_fat32_mount` arriba.

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
- [ ] Mapear framebuffer como memoria write-combining — corregido en Fase 6: actualmente mapeado como RW|User normal (flags 0x07), sin PAT/PCD configurado. Sin efecto práctico en QEMU.
- [x] Implementar consola grafica simple sobre framebuffer — ahora con scroll real (ver Fase 6).
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
- [ ] Usar NVIDIA para inferencia LLM solo despues de tener driver MMIO/VRAM/colas estable; CUDA/driver propietario no aplica dentro de MicroK.

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

- [x] Definir IPv4 header, checksum y validacion.
- [x] Recibir paquetes IPv4 dirigidos a la IP local.
- [x] Enviar paquetes IPv4 a IP local o via gateway.
- [x] Implementar ICMP echo reply.
- [x] Agregar `ping <ip>` como primer test end-to-end.

#### Etapa 4: IP estatica configurable

- [x] Parser de `/microk/net.cfg` con claves: `mode`, `ip`, `netmask`, `gateway`, `dns`.
- [x] Aplicar config al boot si existe.
- [x] Exponer `net status` con MAC, modo, IP, netmask, gateway y link.
- [x] Guardar cambios de shell en FAT32 cuando el FS permita escritura — comando `net config save` (syscall 53) persiste modo/ip/netmask/gateway/dns/hostname/llm_net/llm_port a `/net.cfg` en el mismo formato que `net_load_config_text()` parsea; usa `fat32_create_file`+`fat32_write_file` si el archivo no existe aún.
- [x] Tests de parser host-side - `scripts/net_config_parser_test.c` reimplementa `net_load_config_text()` y sus helpers (`parse_ipv4`, `copy_token`, `parse_u16`, etc.) contra setters mock, ya que el `net.c` real depende de hardware (e1000/DHCP) para compilar. Cubre config estatica completa, DHCP, config estatica incompleta (no se aplica), IPs invalidas, lineas sin `=`/claves desconocidas (se ignoran sin abortar el parseo), `llm_port` invalido o fuera de rango uint16, espacios/CRLF tolerados, lineas en blanco/comentarios, e input vacio/NULL. Target `make test-netcfg` compila y corre.

#### Etapa 5: DHCP cliente

- [x] Implementar UDP minimo.
- [x] Implementar DHCP Discover/Offer/Request/Ack.
- [x] Configurar IP, netmask, gateway, DNS y lease time desde DHCP.
- [x] Reintentos con timeout y fallback a config estatica si existe.
- [x] Comando `dhcp renew`.

#### Etapa 6: DNS y utilidades (DONE)

- [x] Resolver DNS A record basico.
- [x] Agregar `nslookup <host>`.
- [x] **Preparar base para HTTP simple o descarga de modelos (requiere TCP)** - Experimental; cliente TCP minimo de una sola conexion (`kernel/net.c`, seccion "TCP client"), suficiente para hacer un GET HTTP/1.0 bloqueante — no es una API de sockets general. `net_send_tcp_segment()`/`net_handle_tcp()` implementan el handshake de 3 vias, envio/recepcion de datos y cierre activo (FIN/ACK, sin TIME_WAIT); checksum con pseudo-header via `net_checksum16_partial`/`net_checksum16_finish` (a diferencia de UDP, el checksum de TCP es obligatorio, no opcional). `net_http_get(host, port, path, out, out_size)` resuelve el host (reusa `net_dns_resolve`, acepta IPs dotted-quad directo), conecta, manda `GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n`, y copia la respuesta cruda (headers+body, sin parsear) hasta que el servidor cierra o el buffer se llena. Expuesto por el comando de shell `http get <host> <path>` (puerto 80 fijo desde el shell; la syscall 63 sí permite puerto arbitrario) via un struct de pedido con strings de tamaño fijo (`host[64]`+`path[128]`) en vez de punteros anidados, para que la syscall solo necesite validar una región contigua de memoria de usuario.
- [x] **Bug encontrado y corregido: timeout de conexion TCP colgaba el boot para siempre** - El primer intento de verificacion invocaba `net_http_get()` directamente desde `main()` antes de `task_create(net_poll_task)`/`jump_usermode()` — exactamente la misma trampa que ya se documento para UHCI en el roadmap de USB: `timer_get_ticks()` no avanza hasta que la primera IRET a Ring 3 habilita interrupciones, asi que cualquier `while (timer_get_ticks() < deadline) { ... }` corrido antes de eso jamas termina. Fix: la verificacion se movio a correr dentro de `net_poll_task()` (que ya corre correctamente post-scheduler), sin cambio en el diseño de `net_http_get()` en si.
- [x] **Bug encontrado y corregido: el anillo RX del e1000 se atascaba permanentemente despues del primer paquete no-ARP** - Con el bug resuelto arriba, la conexion TCP seguia fallando (rc=-2, timeout de handshake) a pesar de que capturas de paquete (`-object filter-dump`) confirmaban que QEMU/SLIRP respondia con un SYN-ACK valido. Instrumentacion con `-trace 'e1000_receiver_overrun,e1000x_rx_*'` revelo `Receiver overrun: dropped packet of 60 bytes, RDH=1, RDT=1` — el modelo e1000 de QEMU (`hw/net/e1000.c`, `e1000_has_rxbufs`) trata `RDH == RDT` como "cero descriptores disponibles" para paquetes cortos. El codigo existente escribia `RDT` con el indice *post-incremento* de `e1000_rx_cur` al liberar un descriptor — pero el propio `RDH` de hardware avanza a ese mismo indice apenas termina de entregar un paquete, asi que la primera vez que software libera un descriptor, `RDH` y `RDT` quedan exactamente iguales: un atasco permanente, no solo un anillo con capacidad reducida (nada vuelve a mover `RDT` una vez atascado, asi que ningun paquete posterior es aceptado nunca mas). Coincide con el "remaining bug... may still time out" que el README ya documentaba para el smoke test UDP, y contradice el checkbox "e1000 RDT ring fix" marcado [x] en la Etapa 7 de LLM remoto mas abajo — ese fix anterior aparentemente no resolvio el caso general. Fix real: escribir `RDT` con el indice *pre-incremento* (el descriptor recien liberado), no el post-incremento — preserva el mismo invariante que el `RDT = E1000_RX_DESC_COUNT - 1` de la inicializacion del anillo (con `RDH = 0`) en cada paquete subsiguiente, en vez de colapsarlo despues del primero.
- [x] **Bug encontrado y corregido: el padding minimo de Ethernet se contaba como payload real de TCP** - Incluso con el fix del anillo RX, el stream TCP llegaba corrompido — logueando byte a byte se veia el primer segmento (un ACK puro del handshake, sin datos) reportando `payload_len=6` en vez de `0`, lo cual desincronizaba el tracking de numero de secuencia para el resto de la conexion. Causa: Ethernet exige un frame minimo de 60 bytes, así que cualquier payload real mas corto llega paddeado con ceros; `net_handle_packet()` calculaba `ip_payload_len` como los bytes crudos restantes del frame (incluyendo ese padding) en vez de usar el largo total declarado por el propio header IP. UDP ya evita este problema porque tiene su propio campo `udp->len` autodescriptivo (usado en vez del largo crudo); TCP no tiene un campo asi y depende enteramente del header IP para saber su tamaño real, igual que ICMP. Fix: `net_handle_packet()` ahora recorta `ip_payload_len` al valor declarado en `ip->len` (menos el header IP) cuando ese valor es mas chico que los bytes crudos del frame, beneficiando a ICMP y TCP por igual.
- [x] **Verificado en QEMU real** (`make qemu-net`-style boot, `-netdev user,id=net0 -device e1000,netdev=net0`, con un `python3 -m http.server 8000` corriendo en el host WSL): un GET temporal a `10.0.2.2:8000` — SLIRP expone el propio host de QEMU en esa IP — devolvio la respuesta HTTP/1.0 completa y correcta (`HTTP/1.0 200 OK`, headers, y el cuerpo real `hello from microk test server`), confirmando handshake, transferencia de datos multi-paquete, y cierre limpio end-to-end. Sin regresiones: boot completo hasta el shell con red estatica/DHCP configurada sigue funcionando, y el flujo de `make qemu-usb` (no relacionado, pero comparte `storage_init()`) tampoco se vio afectado.

#### Etapa 7: Servicio remoto directo al LLM

Prioridad aprobada: antes de SSH, MicroK debe poder hablar directamente con su
servicio LLM por red. SSH queda para una etapa posterior porque requiere TCP
robusto, criptografia, claves, random seguro y autenticacion madura.

- [x] Definir protocolo textual minimo: `PING`, `STATUS`, `INFO`, `ASK <prompt>`.
- [x] Implementar servicio UDP stateless para `llm ask` end-to-end; e1000 RDT ring fix + net_send_udp() completan el ciclo RX→handler→TX. **Nota agregada luego (Etapa 6 de Red, TCP/HTTP)**: este fix de RDT resolvia el caso puntual probado en su momento, pero el invariante general seguia roto — cualquier paquete no-ARP recibido *despues* del primero quedaba atascado permanentemente (`RDH == RDT`). Probablemente causaba los timeouts intermitentes de UDP que el README documentaba como pendientes. Fix definitivo (escribir `RDT` con el indice pre-incremento) en la seccion de TCP/HTTP mas abajo.
- [x] Agregar puerto configurable para el servicio LLM.
- [x] Limitar tamano de prompt y respuesta para evitar overflow.
- [x] Agregar token simple opcional para laboratorio: prefijo `<token> <cmd>` en cada request.
- [x] Agregar comando `llm net on/off/status` y `llm net port <puerto>`.
- [x] Crear script host-side `scripts/llm_udp_client.py` para probar desde el host.
- [x] Documentar prueba QEMU smoke: host envia `PING`, ARP responde, UDP RX posterior queda como bug abierto.
- [x] Documentar prueba QEMU final: host envia `ASK hola`, MicroK responde por UDP (RDT fix + net_send_udp completan ciclo).

#### Etapa 8: Shell remota liviana

- [x] Implementar consola remota tipo telnet-like solo para QEMU/lab (UDP port 2323).
- [x] Reusar funciones kernel sin duplicar logica: net_status_string, llm_inference, fat32_load_file, etc.
- [x] Protegerla detras de flag/config explicito (`rsh on/off`).
- [x] Agregar autenticacion simple por token antes de aceptar comandos (`rsh token <t>`).
- [x] Mantener SSH como objetivo futuro, no como dependencia temprana.

#### Criterio de listo

- [x] `make qemu-net` levanta MicroK con NIC visible.
- [x] `net status` muestra NIC, MAC e IP.
- [x] Con DHCP disponible, MicroK obtiene IP automaticamente.
- [x] Sin DHCP, MicroK usa `/microk/net.cfg` o config manual.
- [x] `ping <gateway>` responde en QEMU.
- [x] Un cliente UDP externo puede enviar `ASK <prompt>` y recibir respuesta del LLM.

### Roadmap de Arranque en Hardware Real

Objetivo: que MicroK pueda arrancar en una PC real (no solo QEMU) y llegar al
shell interactivo, con storage/red operativos si el hardware especifico lo
permite. Hoy todo el desarrollo y testing es QEMU-first — README/ROADMAP ya
documentan por que: el driver stack actual asume hardware que QEMU emula pero
que las PCs reales dejaron de traer hace mucho (`UHCI` en vez de
OHCI/EHCI/xHCI, `ATA PIO` en vez de AHCI/NVMe, `Intel e1000` en vez de
chipsets de red modernos), mas un boot solo-BIOS/CSM sin path UEFI nativo. Este
no es un fix incremental: cada etapa de abajo es, en tamaño, comparable a un
driver stack nuevo entero (USB o red lo fueron), y a diferencia de esos casos
no hay forma de verificar en QEMU si el fix realmente sirve en hardware real —
cada etapa que toque hardware real necesita acceso a una maquina fisica para
confirmarse, no solo revisar que compile. Por eso se aborda en etapas chicas,
cada una con su propio criterio de verificacion, igual que USB/red/NVIDIA.

#### Etapa 0: Auditoria y decisiones previas (Pendiente)

**Nota: VirtualBox como paso intermedio antes de hardware real.** A
diferencia de QEMU, VirtualBox no tiene un equivalente al flag `-kernel` —
necesita un medio booteable (ISO/disco virtual) con GRUB instalado
cadeneando al kernel Multiboot v1, exactamente el mismo mecanismo que la
Etapa 1 describe para una PC real. Ademas su hardware virtual difiere de
QEMU justo en los puntos que le importan a este roadmap: soporta un
controlador IDE PIIX3/PIIX4 (no solo AHCI/SATA, asi que `kernel/ata.c` PIO
tiene chance real de andar tal cual) y emula explicitamente la familia
Intel PRO/1000 (82540EM/82545EM, es decir e1000 — el driver actual podria
funcionar sin cambios). Para USB en cambio VirtualBox solo emula
OHCI/EHCI/xHCI, nunca UHCI, asi que sirve como prueba negativa gratis: el
driver actual no va a encontrar nada, igual que en una PC real, sin
necesitar hardware fisico para confirmarlo. En resumen: boot, storage y red
tienen buena chance de andar en VirtualBox sin escribir codigo nuevo; USB
no, y probarlo ahi ya valida ese gap antes de conseguir hardware real.

- [x] **Probar el boot (Etapa 1, GRUB + Multiboot v1 en un medio booteable) en VirtualBox primero** - Verificado; ver bloque de hallazgos abajo. `make iso` (nuevo target, Makefile) genera un ISO GRUB2 rescue booteable (`grub-mkrescue`, requiere `grub-pc-bin`+`xorriso`) que arranca MicroK en VirtualBox vía CSM/BIOS legado real, sin usar el atajo `-kernel` de QEMU.
- [x] **Con esa VM, probar el controlador IDE (PIIX3/PIIX4)** - Verificado; `kernel/ata.c` funciona sin cambios contra el controlador IDE PIIX de VirtualBox, con una condición: el disco debe estar en un **controlador IDE**, no en el SATA/AHCI que VirtualBox usa por default para discos nuevos (`ata.c` es PIO puro sobre `0x1F0`, no habla AHCI). El adaptador de red Intel PRO/1000 (`kernel/net.c`) todavía no se probó en esta VM.
- [ ] Probar el adaptador de red Intel PRO/1000 (`kernel/net.c`) en esta misma VM — pendiente, no se llegó a probar en esta sesión.

**Hallazgos de la primera verificación real en VirtualBox (`microk` VM, Base Memory variable, GRUB via `make iso`):**

- [x] **Bug encontrado y corregido: GRUB no puede negociar el modo de video VBE pedido por el header Multiboot, aborta el boot entero** - `kernel/boot.asm` pedía un framebuffer lineal 800x600x32 vía el bit GRAPHICS del header Multiboot v1 (`flags=0x00000007`). Bajo QEMU esto lo maneja el loader `-kernel` propio de QEMU sin problema, pero GRUB real (probado con Graphics Controller tanto VMSVGA como VBoxVGA en VirtualBox) falla con `error: unsupported graphical mode type <valor basura>` al intentar leer el bloque de info de modo VBE vía BIOS — un bug conocido de la interacción GRUB/VBE en VirtualBox, no algo especifico de este kernel. El error aborta el comando `multiboot` sin cargar nada, y el `boot` posterior falla con `error: you need to load the kernel first`. Fix: `MICROK_NO_VIDEO_MODE` (define de nasm, `kernel/boot.asm`) quita el bit GRAPHICS del header cuando se define, sin tocar el build normal. Como `kernel/video.c` ya hacia fallback a VGA texto cuando `mbi->flags` no trae framebuffer info (bit usado para VBE control info, no confundir con el bit de framebuffer real — ver siguiente item de camino a verificar mas a fondo), este build alternativo simplemente nunca pide el modo grafico y bootea directo a VGA texto. Nuevo target `make iso` en el Makefile compila esta variante (`kernel/boot_bios.o` -> `build/kernel-bios.bin`) separada del `build/kernel.bin` normal que siguen usando los targets `qemu-*` (sin cambios, sin regresion).
- [x] **Bug encontrado y corregido: PS/2 typematic (auto-repeat) del controlador dejaba varios caracteres por una sola pulsacion** - `kernel/keyboard.c` seteaba `last_char` en cada scancode de "make" sin ningun debounce, dependiendo por completo del ritmo de auto-repeat propio del hardware PS/2 para no inundar el buffer de linea del shell. El valor por default del controlador (potencialmente ~30Hz tras solo 250ms de delay) nunca fue un problema bajo QEMU (los tests de entrada eran pulsaciones cortas y discretas), pero una pulsacion humana normal bajo la emulacion PS/2 de VirtualBox alcanzaba a generar varios repeats antes de soltar la tecla (ej. `s` -> `ssss`). Fix: `keyboard_init()` (`kernel/keyboard.c`) ahora manda el comando `0xF3` (Set Typematic Rate/Delay) con el byte `0x7F` (rate mas lento ~2Hz, delay mas largo 1000ms) al dispositivo PS/2 al boot, sondeado directamente sin depender de IRQs (mismo razonamiento que las trampas de `timer_get_ticks()` ya documentadas para UHCI/red: a esta altura del boot las interrupciones todavia no estan habilitadas globalmente).
- [x] **Hallazgo (no bug de este kernel): un disco nuevo agregado al controlador SATA por default de VirtualBox no es visible para `ata.c`** - Al adjuntar el disco de storage (`storage.img`, convertido a `.vdi` via `VBoxManage convertfromraw`) al controlador SATA que VirtualBox crea por default, `storage_init()` no lo detecta (`Failed to load model from FAT32/EXT.`, `FAT32 filesystem not mounted.`) porque `ata.c` es PIO puro sobre los puertos IDE legacy (`0x1F0`), no AHCI. Coincide exactamente con el gap ya anticipado en la Etapa 4 de este mismo roadmap ("Storage AHCI minimo — Pendiente"). Solucion (config de VM, no de codigo): agregar un controlador **IDE** en Settings -> Storage y adjuntar el disco ahi como Primary Master en vez de en SATA.
- [x] **Hallazgo (no bug de este kernel): agregar el disco IDE cambio el Boot Device Order de la VM y el BIOS intento bootear directo del disco** - Con "Disco duro" agregado por encima de "Optica" en Settings -> System -> Boot Order, el BIOS bootea el sector de arranque de `storage.img` en vez de la ISO de GRUB. Como `storage.img` solo tiene `mkfs.fat` corrido (sin bootloader instalado), el BIOS ejecuta bytes de la cabecera FAT32 como si fueran codigo x86 real — el resultado es pantalla completa de caracteres/colores aleatorios (llamadas de video BIOS dispersas por casualidad dentro de codigo basura), facilmente confundible con corrupcion de memoria del kernel pero en realidad nunca se llego a ejecutar el kernel. Solucion (config de VM): desmarcar "Disco duro" del Boot Device Order o dejar solo "Optica" marcada — el disco IDE sigue siendo visible para `ata.c` en tiempo de ejecucion sin estar en el boot order del BIOS, son mecanismos independientes.
- [x] **Hallazgo y mitigacion parcial: lecturas ATA PIO sector-por-sector son ordenes de magnitud mas lentas bajo VT-x que bajo QEMU TCG** - `ata_read_sector()`/`ata_write_sector()` (`kernel/ata.c`) hacian un comando ATA completo (select+wait+DRQ-poll+256 lecturas/escrituras de puerto de 16 bits+wait) por cada sector de 512 bytes. Cargar un modelo GGUF de decenas de MB implica cientos de miles de accesos a puertos de I/O. Bajo QEMU con `-accel tcg` cada `in`/`out` se emula en el mismo proceso (barato); bajo VirtualBox con virtualizacion de hardware real (VT-x) cada `in`/`out` es un VM-exit real hacia el hipervisor, ordenes de magnitud mas caro por acceso. **Fix aplicado**: `insw`/`outsw` (`kernel/io.h`, wrappers de `rep insw`/`rep outsw`) reemplazan el loop manual de 256 `inw()`/`outw()` por sector en `ata_read_sector`/`ata_write_sector` — una unica instruccion repetida que un hipervisor puede resolver en un puñado de VM-exits en vez de 256 separados, el mismo patron estandar que cualquier driver ATA PIO real usa por esta misma razon. Verificado en QEMU (`make image-mklm` + boot headless): el autoload de `/model.mklm` via FAT32/ATA en el boot log llega a 100% y monta sin corrupcion, mismo comportamiento que antes del cambio; el path de escritura (`outsw`) es el mismo patron simetrico pero no se re-verifico por separado esta sesion (no hay test automatico de escritura FAT32 en el boot log). **Lo que queda pendiente**: esto solo reduce el overhead de transferencia de datos en si; el overhead de *comando* (select+wait+DRQ-poll, ~10-20 accesos de puerto por sector) sigue pagandose una vez por cada sector de 512 bytes porque `ata_read_sector`/`ata_write_sector` siguen operando de a un sector por comando ATA (`ATA_CMD_READ_PIO`/`ATA_CMD_WRITE_PIO`, `SECCOUNT0=1`) — agrupar en comandos `READ/WRITE MULTIPLE` de N sectores por comando amortizaria ese resto sobre N, pendiente de implementar si el rendimiento en VirtualBox sigue siendo un problema despues de este fix. Sin re-verificar todavia en VirtualBox real (compilado limpio, `microk.iso` regenerado). **Re-verificado en QEMU** (`qemu-system-i386 -kernel build/kernel.bin -drive file=storage.img,...`, sin `-initrd`, forzando el autoload `/model.mklm` via FAT32/ATA en vez de Multiboot module): la barra de progreso llega a 100%, `Loaded LLM Model from FAT32.` y el boot llega limpio al shell — sin regresion tras el cambio a `insw`/`outsw`. Sigue pendiente la verificacion en VirtualBox real (el objetivo original del fix, donde el overhead de VM-exit por puerto es el problema).

- [x] **Batching de comandos ATA: `READ/WRITE SECTORS` con `SECCOUNT0 > 1` en vez de un comando por sector** - Implementado lo que quedaba pendiente arriba, sin llegar a `READ/WRITE MULTIPLE` (0xC4/0xC9, que requiere negociar el tamaño de bloque via `SET MULTIPLE MODE` e IDENTIFY word 47): `ata_read_sectors()`/`ata_write_sectors()` (`kernel/ata.c`) emiten un unico comando `READ_PIO`/`WRITE_PIO` (0x20/0x30) con `SECCOUNT0` de hasta 255 sectores, sondeando DRQ una vez por sector dentro del mismo comando (DRQ sigue asertandose por sector con estos comandos clasicos, a diferencia de READ/WRITE MULTIPLE que agrupa DRQ por bloque) — el ahorro es el overhead de *comando* (select+LBA+wait+issue) que antes se pagaba una vez por sector y ahora se paga una vez cada hasta 255 sectores. `ata_read_sector`/`ata_write_sector` (usados por el camino de escritura parcial read-modify-write) ahora son wrappers de una linea sobre las nuevas funciones con `count=1`, sin cambio de comportamiento. `ata_block_read`/`ata_block_write` detectan corridas alineadas de sectores completos (offset%512==0 y tamaño restante >=512) y las despachan directo al buffer del llamador via el nuevo camino batched; los sectores parciales de punta/cola siguen el mismo read-modify-write de siempre, sin tocar. **Verificado en QEMU**: `loadmodel stories15M.gguf` (~98MB, ~192000 sectores, agrupados en ~753 comandos de hasta 255 sectores en vez de ~192000 comandos de 1) completa la barra de progreso a 100% y `llm selftest gguf` corre inferencia real sobre el modelo recien cargado y devuelve `PASS (tokens generated)` — confirma integridad de los datos leidos via el camino batched, no solo que "no crashea". El camino de escritura parcial (sin cambios de codigo, ruta no alcanzable por el batching ya que el buffer del comando `write` de la shell esta limitado a la linea de entrada de 64 bytes) se re-confirmo con un roundtrip `touch`+`write`+`cat` exacto. Sin re-verificar en VirtualBox real (el motivador original de todo este hallazgo); el camino de escritura alineada multi-sector tampoco tiene todavia un test que fuerce mas de un sector por comando (ningun comando de shell actual escribe >512 bytes de una sola vez), queda cubierto solo por lectura de modelos grandes por ahora.
- [x] **Bug encontrado, sin resolver, mitigado por configuracion: RAM configurada en o cerca de exactamente 4096MB dispara corrupcion severa (pantalla llena de datos binarios del modelo interpretados como caracteres de video) al terminar de cargar un modelo** - Con Base Memory=4096MB, cargar `stories15M.gguf` terminaba con la pantalla completa de caracteres/colores aparentemente aleatorios (visualmente identico al hallazgo de boot-device-order de arriba, pero esta vez con el kernel corriendo — el shell seguia respondiendo antes de esto). Hipotesis de causa raiz, no confirmada: los chipsets de PC suelen reservar un hueco de MMIO por debajo de la linea de 4GB, remapeando una porcion de la RAM configurada por encima de 4GB — con esto, RAM≈4096MB puede ser la primera configuracion en toda la vida de este proyecto que efectivamente activa el camino de alta memoria/PAE (`kernel/highmem.c`, `kernel/vmm_pae.c`) fuera de los tests dedicados (`highmemtest`). Ese camino ya tiene un bug documentado y **sin causa raiz identificada** en la seccion "Roadmap de RAM Completa" mas arriba (migracion de tensores GGUF a memoria alta corrompiendo datos de forma rara y reproducible bajo QEMU) — es plausible que el mismo bug, bajo timing/layout de memoria distinto de VirtualBox, se manifieste de forma mucho mas severa. **Mitigacion verificada**: bajar Base Memory a 2048MB (todavia muy por encima de los 768MB que este modelo necesita segun el target `qemu-stories15`) elimina el problema por completo.
- [x] **Verificado end-to-end en VirtualBox real** (`make iso` + disco IDE con `storage.vdi` conteniendo `stories15M.gguf`, Base Memory=2048MB, Graphics Controller=VBoxVGA, Boot Order solo Optica): boot completo hasta el shell interactivo en Ring 3, teclado PS/2 funcional sin repeats espurios, `ls`/`loadmodel`/`llm ask hello` encuentran y montan el FAT32 del disco IDE, y `llm ask hello` genera 32 tokens de texto real (mezcla de palabras reales y fragmentos, calidad comparable a la ya documentada en QEMU para este mismo modelo — sin evidencia de regresion). Primera vez que este proyecto corre fuera de QEMU.
- [x] **Bug encontrado y corregido: todas las syscalls corren con interrupciones globalmente deshabilitadas por toda su duracion, congelando timer y teclado durante `llm ask`** - `isr128` (`kernel/interrupts.asm`, la puerta de `int 0x80`) esta registrada como interrupt gate (`idt_set_gate(128, ..., 0xEE)` en `kernel/idt.c`): la CPU limpia `EFLAGS.IF` al entrar y nada la restaura hasta el `iret` final de la syscall — no hay un solo `sti` en toda la base de codigo. Para syscalls cortas esto es inofensivo, pero `llm ask` corre su bucle de generacion completo (potencialmente varios segundos) adentro de una unica syscall, dejando IRQ0 (timer) e IRQ1 (teclado) completamente inhibidas durante toda la generacion. Sintomas observados: `[LLM Report] Generated N tokens in 0 ms` (los ticks nunca avanzan mientras se mide), y en VirtualBox, tipear durante `llm ask` hace que el teclado PS/2 host-side falle con `VERR_PDM_NO_QUEUE_ITEMS` (las teclas se acumulan esperando que el guest drene IRQ1, que nunca corre, hasta desbordar la cola interna de VirtualBox). Ademas invalida en la practica la funcion de aborto por Ctrl+C durante generacion (`sys_abort_requested()`, ya documentada como Estable en la Etapa 8 de UX) y el timeout por ticks (`llm_generation_timed_out`) — ambos dependen de que IRQ1/IRQ0 puedan correr, y ninguno de los ~12 puntos de chequeo ya existentes en el forward pass/scan de vocabulario (`llm_generation_cancelled`, llamado desde `kernel/llm.c`) podia realmente observarlos. Fix acotado: `llm_generation_cancelled()` (el unico punto por el que pasan todos esos chequeos) ahora abre una ventana de una instruccion (`sti; nop; cli`, aprovechando el "shadow" post-STI de x86 para garantizar que al menos una interrupcion pendiente pueda despacharse antes del `cli`) cada vez que se llama. Deliberadamente **no** se toco `isr128` en general — agregar `sti` ahi haria **todas** las syscalls preemptibles (incluyendo las que tocan estructuras globales sin lock como `ata.c`/`fat32.c`/`kheap.c`), un cambio de alcance mucho mayor y con riesgo real de introducir carreras nuevas; el fix se limito al unico camino conocido que corre lo bastante largo para importar. Pendiente de verificacion en VirtualBox real (compilado limpio, sin probar todavia si el error de teclado desaparece y si Ctrl+C ahora interrumpe una generacion en curso). **Re-verificado en QEMU** (`make qemu-stories15`, automatizando `llm ask hola` via `sendkey` sobre el monitor HMP para no depender de PS/2 real): `[LLM Report] Generated 32 tokens in 26730 ms.` — tiempo real no nulo (antes de este fix habria impreso `0 ms` con los ticks congelados durante toda la syscall), y el shell vuelve a responder con normalidad al prompt inmediatamente despues, sin colgarse. Confirma que la ventana `sti/nop/cli` deja pasar IRQ0 durante la generacion tal como se esperaba. Sigue pendiente confirmar en VirtualBox real que esto tambien resuelve el error de teclado `VERR_PDM_NO_QUEUE_ITEMS` y que Ctrl+C interrumpe una generacion en curso.
- [ ] Confirmar que el hardware objetivo (la PC real disponible para probar)
  soporta boot BIOS/CSM heredado, o si es UEFI-only sin fallback — esto
  decide si la Etapa 1 es alcanzable tal cual o si la Etapa 2 (UEFI) es
  prerequisito, no una mejora posterior.
- [ ] Confirmar controlador USB real disponible (`lspci`/Device Manager en el
  hardware objetivo) para saber si vale la pena una Etapa 3 (xHCI) minima o
  si conviene apuntar a otro objetivo.
- [ ] Confirmar controlador de disco real disponible (AHCI es virtualmente
  universal en PCs de la ultima decada, pero confirmar si hay modo IDE/legacy
  disponible en BIOS como atajo antes de escribir un driver AHCI nuevo).
- [ ] Confirmar chipset de red real disponible; si no es compatible, definir
  si esta etapa se prueba sin red (aceptable, ya que shell/storage no
  dependen de ella).
- [ ] Decidir la superficie minima de "listo": ¿alcanza con boot + shell +
  teclado, o hace falta storage/red reales para considerarlo un exito?

#### Etapa 1: Pendrive booteable via BIOS/CSM, sin drivers nuevos (Pendiente)

Probar el path mas barato primero: usar el boot BIOS/legacy que ya existe
(README: "BIOS/GRUB/QEMU style boot flow") sin escribir ningun driver nuevo,
para ver hasta donde llega el codigo actual en metal real.

- [ ] Instalar GRUB2 en un pendrive USB con una entrada Multiboot v1 apuntando
  a `build/kernel.bin`, replicando a mano lo que `make image-*` arma para las
  imagenes de QEMU.
- [ ] Arrancar la PC real con boot BIOS/CSM habilitado (no UEFI) desde ese
  pendrive.
- [ ] Documentar exactamente donde se detiene o que funciona: ¿GDT/IDT/PMM
  arrancan? ¿VGA texto se ve? ¿PS/2 responde (si la PC tiene puerto PS/2
  real, cada vez menos comun) o el teclado USB emulado por la BIOS como PS/2
  ("USB Legacy Support") funciona con el driver PS/2 existente?
- [ ] Si crashea, capturar el estado exacto (excepcion, registros) via el
  panic handler existente (`kernel/idt.c`) en vez de adivinar — este proyecto
  ya tiene precedente de bugs que solo aparecen fuera de QEMU (alineación de
  stack/SSE, DMA/bus-mastering) y de diagnostico agregado especificamente
  para encontrarlos.

#### Etapa 2: Boot UEFI nativo (Pendiente)

Solo necesaria si la Etapa 0/1 confirma que el hardware objetivo no tiene
CSM/legacy boot disponible — cada vez mas comun en PCs nuevas.

- [ ] Evaluar GRUB2 en modo UEFI cargando el kernel Multiboot v1 actual via
  chainload (mas simple, sin tocar `kernel/boot.asm`) contra migrar a
  Multiboot2 con entrada UEFI nativa (mas trabajo, menos dependencia de
  GRUB).
- [ ] Si hace falta Multiboot2: nuevo header de boot, y revisar que
  `boot_modules.c`/`main.c` sigan recibiendo el mapa de memoria en un formato
  que `pmm.c` entienda (Multiboot2 lo estructura distinto a Multiboot v1).
- [ ] Verificar que el framebuffer entregado por UEFI GOP (en vez de VESA/VBE
  BIOS) sea compatible con el codigo de framebuffer existente
  (`kernel/video.c`) o necesite una ruta de deteccion separada.

#### Etapa 3: Controlador USB moderno minimo (Pendiente)

El driver UHCI actual (`kernel/uhci.c`) no sirve en hardware con solo
xHCI/EHCI — casi todas las PCs de los ultimos 10-15 años. Adaptar el enfoque
por etapas ya usado para UHCI (deteccion → bring-up → enumeracion → Bulk-Only
Transport) en vez de intentar todo de una vez.

- [ ] Elegir el objetivo minimo: xHCI (USB 3.x, el mas comun en hardware
  reciente pero el protocolo mas complejo de los tres) vs EHCI (USB 2.0, mas
  simple, y presente como "companion controller" en casi cualquier placa con
  xHCI) — probablemente EHCI primero por ser mas chico, con xHCI como etapa
  separada despues.
- [ ] Deteccion via PCI (mismo patron que `uhci_find_controller`), pero con
  BARs de tipo MMIO en vez de I/O ports puros — el kernel no tiene todavia
  mapeo de BAR MMIO generico reusable fuera de casos puntuales (e1000, GPU),
  revisar si conviene generalizarlo.
- [ ] Bring-up minimo + deteccion de puertos, mismo criterio de verificacion
  que UHCI Etapa 0+1 (log automático, sin crashear con o sin dispositivo).
- [ ] Enumeracion + Bulk-Only Transport + block_device_t: reusar el diseño
  ya probado de `kernel/uhci.c` (BOT/SCSI son identicos sobre cualquier host
  controller; solo cambia la capa de transferencias de bajo nivel) en vez de
  reescribirlo.

#### Etapa 4: Storage AHCI minimo (Pendiente)

`kernel/ata.c` (PIO puro) no existe en casi ningun disco real moderno
configurado en modo AHCI (el default de fabrica en la enorme mayoria de PCs).

- [ ] Confirmar si el BIOS objetivo ofrece modo IDE/legacy como atajo antes
  de escribir un driver nuevo — si existe, esta etapa podria no ser
  necesaria para una primera demo.
- [ ] Si hace falta AHCI real: deteccion PCI, mapeo de BAR5 (ABAR) MMIO,
  inicializacion de puertos, comandos FIS minimos para IDENTIFY/READ/WRITE —
  comparable en alcance a la Etapa 0+1 de USB, mismo criterio incremental.
- [ ] Envolver como `block_device_t` igual que `ata_primary_master()` y
  `uhci_msd_primary()` — cero cambios en FAT32/ext2/VFS, mismo patron ya
  probado dos veces.

#### Etapa 5: NIC moderno o fallback (Pendiente)

- [ ] Confirmar si el hardware objetivo trae un chipset de red compatible con
  `e1000e`/`igb` (variantes mas nuevas de Intel, protocolo de registros
  similar al e1000 actual) — si es asi, extender `kernel/net.c` podria ser
  incremental en vez de un driver nuevo desde cero. Un chipset no-Intel
  (Realtek, Broadcom, etc., lo mas comun en hardware de consumo) necesitaria
  un driver enteramente distinto, fuera de alcance salvo que se decida
  priorizarlo explicitamente.
- [ ] Si no hay chipset compatible: aceptar que esta etapa queda sin
  resolver para el hardware objetivo — storage/shell no dependen de red, asi
  que no bloquea el resto del roadmap de hardware real.

#### Criterio de listo

- [ ] MicroK arranca en al menos una PC real (no VM) hasta el shell
  interactivo, con teclado funcional.
- [ ] El path de boot usado (BIOS/CSM o UEFI) queda documentado junto con el
  hardware especifico probado (vendor/modelo, no solo "una PC").
- [ ] Al menos un dispositivo de storage real (USB o interno) es accesible
  como `block_device_t` y monta FAT32/ext2 de solo lectura.
- [ ] Cualquier crash o bloqueo encontrado en hardware real queda
  documentado con causa raiz identificada (no solo "no bootea"), siguiendo
  el mismo estandar de las entradas de bug ya documentadas en este roadmap.

## Prioridad Actual

- [x] **Escritura en Disco (ATA)**: Soporte PIO con Cache Flush.
- [x] **Escritura en FAT32/ext2** - Estable; Soporte para creación y expansión de archivos y actualización de clusters.
- [x] **User Mode (Ring 3)**: Shell aislado ejecutandose en Ring 3 con syscalls.
- [x] **Librería Matemática**: Implementada librería Q16.16 con exp/tanh/sigmoid para el backend de tensores.

## Norte del Proyecto

MicroK no busca solo ser una demo, sino un runtime bare-metal resiliente donde la IA
pueda operar con seguridad y persistencia real.
