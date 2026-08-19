#include "llm.h"
#include "llm_gguf.h"
#include "tensor.h"
#include "tokenizer.h"
#include "pmm.h"
#include "video.h"
#include "kheap.h"
#include "vmm.h"
#include "string.h"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t model_type;
    uint32_t vocab_size;
    uint32_t hidden_size;
    uint32_t layer_count;
    uint32_t flags;
    uint32_t data_offset;
    uint32_t data_size;
    char name[32];
} __attribute__((packed)) microk_model_header_t;

typedef struct {
    uint32_t magic;
    uint32_t entry_count;
} __attribute__((packed)) microk_rule_payload_t;

typedef struct {
    uint16_t prompt_len;
    uint16_t response_len;
} __attribute__((packed)) microk_rule_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t vocab_count;
    uint16_t class_count;
    uint32_t reserved;
} __attribute__((packed)) microk_nn_payload_t;

typedef struct {
    uint16_t len;
} __attribute__((packed)) microk_nn_vocab_entry_t;

typedef struct {
    int16_t bias;
    uint16_t response_len;
} __attribute__((packed)) microk_nn_class_entry_t;

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t size;
    const char *name;
    microk_model_header_t *header;
    gguf_info_t gguf;
    int loaded;
    int valid_microk_model;
    int valid_gguf_model;
    int gguf_gen_ready;
} llm_model_t;

typedef enum {
    LLM_BACKEND_NONE = 0,
    LLM_BACKEND_MKRP,     /* Rule-based (Pattern matching) */
    LLM_BACKEND_MKNN,     /* Simple Neural (Classifier) */
    LLM_BACKEND_GGUF_PARSER, /* GGUF Metadata/Tensors detected, but no forward pass */
    LLM_BACKEND_GGUF_GEN,    /* Full Generative Transformer (Future) */
    LLM_BACKEND_UNKNOWN
} llm_backend_t;

#define MICROK_MODEL_MAGIC 0x4D4C4B4D /* MKLM */
#define MICROK_MODEL_VERSION 1
#define MICROK_MODEL_TYPE_RULES 1
#define MICROK_MODEL_TYPE_NN 2
#define MICROK_RULE_PAYLOAD_MAGIC 0x50524B4D /* MKRP */
#define MICROK_NN_PAYLOAD_MAGIC 0x4E4E4B4D /* MKNN */
#define LLM_GGUF_PREVIEW_MAX_TOKENS 48
#define LLM_GGUF_PREVIEW_VOCAB_LIMIT 65536
#define LLM_GGUF_PREVIEW_TIMEOUT_TICKS 3000
#define LLM_GGUF_FIRST_TEXT_TOKEN 3

typedef struct {
    tensor_t *attn_q;
    tensor_t *attn_k;
    tensor_t *attn_v;
    tensor_t *attn_o;
    tensor_t *attn_norm;
    tensor_t *ffn_gate;
    tensor_t *ffn_up;
    tensor_t *ffn_down;
    tensor_t *ffn_norm;
} llama_layer_t;

typedef struct {
    tensor_t *token_embd;
    tensor_t *output_norm;
    tensor_t *output;
    llama_layer_t layers[32]; // Max layers supported for now
} llama_model_weights_t;

static llama_model_weights_t llm_weights = {0};

static llm_model_t llm_model = {0};
static char llm_info_buf[256];
static char llm_name_buf[64];
static char llm_gguf_reason_buf[96];
static char llm_gguf_runtime_reason_buf[128];
static tokenizer_t *llm_tokenizer = NULL;
static int llm_weights_loaded = 0;
static int llm_trace_on = 1;

static llm_backend_t llm_backend() {
    if (!llm_model.loaded) {
        return LLM_BACKEND_NONE;
    }
    if (llm_model.valid_microk_model && llm_model.header->model_type == MICROK_MODEL_TYPE_RULES) {
        return LLM_BACKEND_MKRP;
    }
    if (llm_model.valid_microk_model && llm_model.header->model_type == MICROK_MODEL_TYPE_NN) {
        return LLM_BACKEND_MKNN;
    }
    if (llm_model.valid_gguf_model) {
        return llm_model.gguf_gen_ready ? LLM_BACKEND_GGUF_GEN : LLM_BACKEND_GGUF_PARSER;
    }
    return LLM_BACKEND_UNKNOWN;
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static int bytes_equal_ci(const char *a, const char *b, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return 0;
    }
    return 1;
}

static int starts_with_ci(const char *text, const char *prefix) {
    uint32_t prefix_len = strlen(prefix);
    uint32_t text_len = strlen(text);

    if (prefix_len == 0 || text_len < prefix_len) {
        return 0;
    }
    return bytes_equal_ci(text, prefix, prefix_len);
}

static int prompt_matches_rule(const char *prompt, const char *rule, uint32_t rule_len) {
    uint32_t prompt_len = strlen(prompt);
    if (prompt_len == rule_len && bytes_equal_ci(prompt, rule, rule_len)) {
        return 1;
    }
    if (rule_len == 0 || prompt_len < rule_len) {
        return 0;
    }
    for (uint32_t i = 0; i <= prompt_len - rule_len; i++) {
        if (bytes_equal_ci(prompt + i, rule, rule_len)) {
            return 1;
        }
    }
    return 0;
}

static void copy_response(char *response, const char *source, uint32_t len) {
    uint32_t out = 0;
    while (out < len && out < 127) {
        response[out] = source[out];
        out++;
    }
    response[out] = '\0';
}

static void copy_cstr_response(char *response, const char *source) {
    copy_response(response, source, strlen(source));
}

static void append_str(char *out, uint32_t *pos, uint32_t max, const char *text) {
    while (text && *text && *pos + 1 < max) {
        out[*pos] = *text;
        (*pos)++;
        text++;
    }
    out[*pos] = '\0';
}

static void append_u32(char *out, uint32_t *pos, uint32_t max, uint32_t value) {
    char tmp[16];
    itoa((int)value, tmp, 10);
    append_str(out, pos, max, tmp);
}

static void log_u32(uint32_t value) {
    char tmp[16];
    itoa((int)value, tmp, 10);
    klog(tmp);
}

static void llm_set_gguf_reason(const char *reason) {
    memset(llm_gguf_reason_buf, 0, sizeof(llm_gguf_reason_buf));
    if (reason) {
        strncpy(llm_gguf_reason_buf, reason, sizeof(llm_gguf_reason_buf) - 1);
    }
}

static void llm_set_gguf_runtime_reason(const char *reason) {
    memset(llm_gguf_runtime_reason_buf, 0, sizeof(llm_gguf_runtime_reason_buf));
    if (reason) {
        strncpy(llm_gguf_runtime_reason_buf, reason, sizeof(llm_gguf_runtime_reason_buf) - 1);
    }
}

/* SentencePiece tokenizers (used by Llama2/TinyStories-style GGUF models)
 * encode a leading word-boundary space as U+2581 "LOWER ONE EIGHTH BLOCK"
 * (UTF-8 bytes E2 96 81) at the start of a token, e.g. token "\xE2\x96\x81the"
 * means " the". Without converting this back to a literal space, every
 * word-start token looks non-ASCII to trace_token_printable() and gets
 * rejected from the argmax — only word-continuation fragments (already
 * plain ASCII) remain selectable, which glues subwords together with no
 * spaces ("erazinenseparogonzo"). Convert once per token before any
 * printability/usability check or before emitting it to the response. */
static const char *llm_token_display(const char *raw, char *buf, uint32_t bufsize) {
    if (!raw) return raw;
    if ((unsigned char)raw[0] == 0xE2 && (unsigned char)raw[1] == 0x96 && (unsigned char)raw[2] == 0x81) {
        uint32_t i = 0;
        buf[0] = ' ';
        while (raw[3 + i] && i + 1 < bufsize) {
            buf[1 + i] = raw[3 + i];
            i++;
        }
        buf[1 + i] = '\0';
        return buf;
    }
    return raw;
}

static int trace_token_printable(const char *token) {
    if (!token || !token[0]) {
        return 0;
    }
    for (uint32_t i = 0; token[i]; i++) {
        unsigned char c = (unsigned char)token[i];
        if (c < 32 || c > 126) {
            return 0;
        }
    }
    return 1;
}

static int llm_preview_token_usable(const char *token) {
    uint32_t visible_count = 0;

    if (!trace_token_printable(token)) {
        return 0;
    }
    if (token[0] == '<' || token[0] == '[') {
        return 0;
    }

    int has_visible = 0;
    for (uint32_t i = 0; token[i]; i++) {
        unsigned char c = (unsigned char)token[i];
        if (c > 32 && c < 127) {
            has_visible = 1;
            visible_count++;
        }
    }
    (void)visible_count;
    if (!has_visible) {
        return 0;
    }
    return 1;
}

static int llm_generation_timed_out(uint32_t start_ticks) {
    extern uint32_t timer_get_ticks();
    return timer_get_ticks() - start_ticks > LLM_GGUF_PREVIEW_TIMEOUT_TICKS;
}

static int llm_generation_cancelled(uint32_t start_ticks) {
    extern int sys_abort_requested();
    // int 0x80 (kernel/interrupts.asm, isr128) is an interrupt gate: the CPU
    // clears IF on entry and nothing re-enables it until the syscall's own
    // iret, so a long-running syscall like GGUF generation runs with IRQs
    // globally masked for its entire duration. That silently broke both
    // Ctrl+C abort and the tick-based timeout below -- sys_abort_requested()
    // can never observe a keypress (IRQ1 can't fire to set it) and
    // timer_get_ticks() never advances (IRQ0 can't fire either), for as
    // long as generation runs. Went unnoticed under QEMU; surfaced on real
    // hardware/VirtualBox as "[LLM Report] Generated N tokens in 0 ms."
    // (ticks frozen) and, more visibly, the host's PS/2 keyboard emulation
    // erroring with VERR_PDM_NO_QUEUE_ITEMS -- typed keys queue up waiting
    // for the guest to drain IRQ1 and eventually overflow VirtualBox's
    // internal delivery queue. This is checked from every hot loop in the
    // forward pass/vocab scan already (see call sites below), so a single
    // one-instruction interrupt window here -- STI, one instruction inside
    // the guaranteed post-STI shadow, then CLI -- gives pending IRQ0/IRQ1 a
    // chance to actually run each time, without making syscalls preemptible
    // in general (every other syscall keeps the prior atomic-for-its-
    // duration behavior; this only touches the one path known to run long
    // enough for it to matter).
    asm volatile("sti\n\tnop\n\tcli" ::: "memory");
    return sys_abort_requested() || llm_generation_timed_out(start_ticks);
}

static void llm_generation_cancel_response(char *response, uint32_t start_ticks) {
    extern int sys_abort_requested();
    if (sys_abort_requested()) {
        copy_cstr_response(response, "LLM: Generation aborted by user.");
    } else if (llm_generation_timed_out(start_ticks)) {
        copy_cstr_response(response, "LLM: GGUF generation timed out before producing a token. Try llm trace off, a shorter prompt, or a smaller model.");
    } else {
        copy_cstr_response(response, "LLM: GGUF generation stopped.");
    }
}

static int llm_gguf_generation_token_limit(void) {
    uint32_t n_layer = llm_model.gguf.arch.n_layer;
    uint32_t n_embd = llm_model.gguf.arch.n_embd;

    /* gguf_probe is now cached per (start,size) (see llm_gguf.c), so tensor
     * lookups no longer rescan all metadata per token. These caps used to
     * compensate for that O(n_vocab) cost and were far too conservative
     * (8 tokens for a 15M model) — raised now that the real bottleneck is gone. */
    if (n_layer >= 8 || n_embd >= 512) {
        return 16;
    }
    if (n_layer >= 4 || n_embd >= 256) {
        return 32;
    }
    return LLM_GGUF_PREVIEW_MAX_TOKENS;
}

static void trace_log_token_text(const char *prefix, uint32_t token_id, const char *token_text) {
    char buf[96];
    uint32_t pos = 0;

    append_str(buf, &pos, sizeof(buf), prefix);
    append_u32(buf, &pos, sizeof(buf), token_id);
    if (token_text && token_text[0]) {
        append_str(buf, &pos, sizeof(buf), " -> \"");
        append_str(buf, &pos, sizeof(buf), trace_token_printable(token_text) ? token_text : "<non-printable>");
        append_str(buf, &pos, sizeof(buf), "\"");
    }
    append_str(buf, &pos, sizeof(buf), "\n");
    klog(buf);
}

static void llm_trace_prompt_tokens(const char *prompt, tokenizer_t *tokenizer) {
    uint32_t tokens[32];
    int n_tokens;
    char buf[96];
    uint32_t pos = 0;

    if (!llm_trace_on || !tokenizer || !prompt) {
        return;
    }

    n_tokens = tokenizer_encode(tokenizer, prompt, tokens, 32);
    append_str(buf, &pos, sizeof(buf), "LLM TRACE PROMPT: \"");
    append_str(buf, &pos, sizeof(buf), prompt);
    append_str(buf, &pos, sizeof(buf), "\"\n");
    klog(buf);

    if (n_tokens <= 0) {
        klog("LLM TRACE IN: no tokens encoded.\n");
        return;
    }

    for (int i = 0; i < n_tokens; i++) {
        char disp_buf[80];
        const char *disp = llm_token_display(tokenizer_decode(tokenizer, tokens[i]), disp_buf, sizeof(disp_buf));
        trace_log_token_text("LLM TRACE IN: ", tokens[i], disp);
    }
}

static void append_model_name(char *out, uint32_t *pos, uint32_t max) {
    if (!llm_model.valid_microk_model) {
        append_str(out, pos, max, llm_model.name);
        return;
    }

    for (int i = 0; i < 32 && llm_model.header->name[i] && *pos + 1 < max; i++) {
        out[*pos] = llm_model.header->name[i];
        (*pos)++;
    }
    out[*pos] = '\0';
}

static microk_nn_payload_t *llm_nn_payload(void) {
    if (!llm_model.valid_microk_model ||
        llm_model.header->model_type != MICROK_MODEL_TYPE_NN ||
        llm_model.header->data_size < sizeof(microk_nn_payload_t)) {
        return 0;
    }

    microk_nn_payload_t *payload = (microk_nn_payload_t *)(llm_model.start + llm_model.header->data_offset);
    if (payload->magic != MICROK_NN_PAYLOAD_MAGIC) {
        return 0;
    }
    return payload;
}

static microk_rule_payload_t *llm_rule_payload(void) {
    if (!llm_model.valid_microk_model ||
        llm_model.header->model_type != MICROK_MODEL_TYPE_RULES ||
        llm_model.header->data_size < sizeof(microk_rule_payload_t)) {
        return 0;
    }

    microk_rule_payload_t *payload = (microk_rule_payload_t *)(llm_model.start + llm_model.header->data_offset);
    if (payload->magic != MICROK_RULE_PAYLOAD_MAGIC) {
        return 0;
    }
    return payload;
}

static int llm_validate_microk_model(uint32_t start, uint32_t size) {
    if (size < sizeof(microk_model_header_t)) {
        return 0;
    }

    microk_model_header_t *header = (microk_model_header_t *)start;
    if (header->magic != MICROK_MODEL_MAGIC) {
        return 0;
    }
    if (header->version != MICROK_MODEL_VERSION) {
        return 0;
    }
    if (header->header_size < sizeof(microk_model_header_t)) {
        return 0;
    }
    if (header->data_offset < header->header_size) {
        return 0;
    }
    if (header->data_offset > size || header->data_size > size - header->data_offset) {
        return 0;
    }
    if (header->model_type == MICROK_MODEL_TYPE_RULES && header->data_size < sizeof(microk_rule_payload_t)) {
        return 0;
    }
    if (header->model_type == MICROK_MODEL_TYPE_NN && header->data_size < sizeof(microk_nn_payload_t)) {
        return 0;
    }
    return 1;
}

static int llm_try_rule_inference(const char *prompt, char *response) {
    if (!llm_model.valid_microk_model || llm_model.header->model_type != MICROK_MODEL_TYPE_RULES) {
        return 0;
    }

    uint8_t *payload_start = (uint8_t *)llm_model.start + llm_model.header->data_offset;
    uint8_t *payload_end = payload_start + llm_model.header->data_size;
    microk_rule_payload_t *payload = (microk_rule_payload_t *)payload_start;
    if (payload->magic != MICROK_RULE_PAYLOAD_MAGIC) {
        return 0;
    }

    uint8_t *cursor = payload_start + sizeof(microk_rule_payload_t);
    for (uint32_t i = 0; i < payload->entry_count; i++) {
        if (cursor + sizeof(microk_rule_entry_t) > payload_end) {
            return 0;
        }

        microk_rule_entry_t *entry = (microk_rule_entry_t *)cursor;
        cursor += sizeof(microk_rule_entry_t);
        if (cursor + entry->prompt_len + entry->response_len > payload_end) {
            return 0;
        }

        char *rule_prompt = (char *)cursor;
        char *rule_response = (char *)(cursor + entry->prompt_len);
        if (prompt_matches_rule(prompt, rule_prompt, entry->prompt_len)) {
            copy_response(response, rule_response, entry->response_len);
            return 1;
        }

        cursor += entry->prompt_len + entry->response_len;
    }
    return 0;
}

static int llm_try_nn_inference(const char *prompt, char *response) {
    if (!llm_model.valid_microk_model || llm_model.header->model_type != MICROK_MODEL_TYPE_NN) {
        return 0;
    }

    uint8_t *payload_start = (uint8_t *)llm_model.start + llm_model.header->data_offset;
    uint8_t *payload_end = payload_start + llm_model.header->data_size;
    microk_nn_payload_t *payload = (microk_nn_payload_t *)payload_start;
    if (payload->magic != MICROK_NN_PAYLOAD_MAGIC ||
        payload->vocab_count == 0 ||
        payload->class_count == 0) {
        return 0;
    }

    uint8_t *cursor = payload_start + sizeof(microk_nn_payload_t);
    char *vocab_words[32];
    uint16_t vocab_lens[32];
    uint8_t features[32];
    uint16_t vocab_count = payload->vocab_count;
    if (vocab_count > 32) vocab_count = 32;

    for (uint16_t i = 0; i < payload->vocab_count; i++) {
        if (cursor + sizeof(microk_nn_vocab_entry_t) > payload_end) return 0;
        microk_nn_vocab_entry_t *entry = (microk_nn_vocab_entry_t *)cursor;
        cursor += sizeof(microk_nn_vocab_entry_t);
        if (cursor + entry->len > payload_end) return 0;

        if (i < vocab_count) {
            vocab_words[i] = (char *)cursor;
            vocab_lens[i] = entry->len;
            features[i] = prompt_matches_rule(prompt, vocab_words[i], vocab_lens[i]) ? 1 : 0;
        }
        cursor += entry->len;
    }

    int32_t best_score = -2147483647;
    char *best_response = 0;
    uint16_t best_response_len = 0;

    for (uint16_t cls = 0; cls < payload->class_count; cls++) {
        if (cursor + sizeof(microk_nn_class_entry_t) > payload_end) return 0;
        microk_nn_class_entry_t *class_entry = (microk_nn_class_entry_t *)cursor;
        cursor += sizeof(microk_nn_class_entry_t);
        if (cursor + class_entry->response_len > payload_end) return 0;

        char *class_response = (char *)cursor;
        cursor += class_entry->response_len;

        if (cursor + (payload->vocab_count * sizeof(int16_t)) > payload_end) return 0;
        int16_t *weights = (int16_t *)cursor;
        cursor += payload->vocab_count * sizeof(int16_t);

        int32_t score = class_entry->bias;
        for (uint16_t i = 0; i < vocab_count; i++) {
            if (features[i]) {
                score += weights[i];
            }
        }

        if (score > best_score) {
            best_score = score;
            best_response = class_response;
            best_response_len = class_entry->response_len;
        }
    }

    if (!best_response || best_score <= 0) {
        return 0;
    }
    copy_response(response, best_response, best_response_len);
    return 1;
}

/**
 * @brief Attempts to perform inference using a GGUF model.
 * Tokenizes the prompt and accesses necessary tensors for computation.
 */
static void llama_build_layer_name(char *out, uint32_t layer, const char *suffix) {
    uint32_t pos = 0;
    append_str(out, &pos, 64, "blk.");
    char tmp[10];
    itoa(layer, tmp, 10);
    append_str(out, &pos, 64, tmp);
    append_str(out, &pos, 64, ".");
    append_str(out, &pos, 64, suffix);
}

static int llm_has_required_gguf_tensor(uint32_t file_start, uint32_t file_size, const char *name) {
    gguf_tensor_t tensor;
    return gguf_get_tensor(file_start, file_size, name, &tensor);
}

static int llm_find_gguf_tensor_alias(uint32_t file_start, uint32_t file_size, const char **aliases, const char **matched_name) {
    if (!aliases) {
        return 0;
    }

    for (uint32_t i = 0; aliases[i]; i++) {
        if (llm_has_required_gguf_tensor(file_start, file_size, aliases[i])) {
            if (matched_name) {
                *matched_name = aliases[i];
            }
            return 1;
        }
    }

    return 0;
}

static tensor_t *llm_load_gguf_tensor_alias(uint32_t file_start, uint32_t file_size, const char **aliases, const char **matched_name) {
    if (!aliases) {
        return 0;
    }

    for (uint32_t i = 0; aliases[i]; i++) {
        if (llm_has_required_gguf_tensor(file_start, file_size, aliases[i])) {
            tensor_t *tensor = tensor_load_gguf(file_start, file_size, aliases[i]);
            if (matched_name) {
                *matched_name = aliases[i];
            }
            return tensor;
        }
    }

    return 0;
}

static int llm_set_missing_runtime_tensor(const char *name) {
    char reason[128];
    uint32_t pos = 0;
    kheap_stats_t heap_stats;

    memset(&heap_stats, 0, sizeof(heap_stats));
    kheap_get_stats(&heap_stats);
    append_str(reason, &pos, sizeof(reason), "Missing runtime tensor ");
    append_str(reason, &pos, sizeof(reason), name);
    append_str(reason, &pos, sizeof(reason), " heap_free_kib=");
    append_u32(reason, &pos, sizeof(reason), heap_stats.free_bytes / 1024);
    llm_set_gguf_runtime_reason(reason);
    return 0;
}

static int llm_set_runtime_tensor_error(const char *name) {
    char reason[128];
    uint32_t pos = 0;
    kheap_stats_t heap_stats;

    memset(&heap_stats, 0, sizeof(heap_stats));
    kheap_get_stats(&heap_stats);
    append_str(reason, &pos, sizeof(reason), tensor_last_error());
    if (name && name[0]) {
        append_str(reason, &pos, sizeof(reason), " tensor=");
        append_str(reason, &pos, sizeof(reason), name);
    }
    append_str(reason, &pos, sizeof(reason), " heap_free_kib=");
    append_u32(reason, &pos, sizeof(reason), heap_stats.free_bytes / 1024);
    llm_set_gguf_runtime_reason(reason);
    return 0;
}

static const char *llm_alias_token_embd[] = { "token_embd.weight", "tok_embeddings.weight", 0 };
static const char *llm_alias_output_norm[] = { "output_norm.weight", "norm.weight", 0 };
static const char *llm_alias_output[] = { "output.weight", "lm_head.weight", "token_embd.weight", 0 };

static int llm_gguf_generation_ready(uint32_t file_start, uint32_t file_size) {
    char name[64];
    uint32_t n_layers;
    const char *matched_name = 0;

    if (!llm_model.valid_gguf_model) {
        llm_set_gguf_reason("GGUF probe failed.");
        return 0;
    }
    if (llm_model.gguf.arch.n_vocab == 0 ||
        llm_model.gguf.arch.n_embd == 0 ||
        llm_model.gguf.arch.n_layer == 0 ||
        llm_model.gguf.arch.n_head == 0) {
        llm_set_gguf_reason("Missing required GGUF architecture metadata.");
        return 0;
    }
    if (!llm_find_gguf_tensor_alias(file_start, file_size, llm_alias_token_embd, &matched_name)) {
        llm_set_gguf_reason("Missing token embedding tensor.");
        return 0;
    }
    if (!llm_find_gguf_tensor_alias(file_start, file_size, llm_alias_output_norm, &matched_name)) {
        llm_set_gguf_reason("Missing output norm tensor.");
        return 0;
    }
    if (!llm_find_gguf_tensor_alias(file_start, file_size, llm_alias_output, &matched_name)) {
        llm_set_gguf_reason("Missing output projection tensor.");
        return 0;
    }

    n_layers = llm_model.gguf.arch.n_layer;
    if (n_layers > 32) n_layers = 32;
    for (uint32_t i = 0; i < n_layers; i++) {
        llama_build_layer_name(name, i, "attn_q.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.attn_q.weight."); return 0; }
        llama_build_layer_name(name, i, "attn_k.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.attn_k.weight."); return 0; }
        llama_build_layer_name(name, i, "attn_v.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.attn_v.weight."); return 0; }
        llama_build_layer_name(name, i, "attn_output.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.attn_output.weight."); return 0; }
        llama_build_layer_name(name, i, "attn_norm.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.attn_norm.weight."); return 0; }
        llama_build_layer_name(name, i, "ffn_gate.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.ffn_gate.weight."); return 0; }
        llama_build_layer_name(name, i, "ffn_up.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.ffn_up.weight."); return 0; }
        llama_build_layer_name(name, i, "ffn_down.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.ffn_down.weight."); return 0; }
        llama_build_layer_name(name, i, "ffn_norm.weight");
        if (!llm_has_required_gguf_tensor(file_start, file_size, name)) { llm_set_gguf_reason("Missing tensor blk.N.ffn_norm.weight."); return 0; }
    }

    llm_set_gguf_reason("Minimum GGUF tensor set detected.");
    return 1;
}

static int llm_load_llama_weights() {
    const char *matched_name = 0;
    char name[64];

    if (!llm_model.valid_gguf_model) return 0;
    llm_set_gguf_runtime_reason("");

    // Load common weights
    llm_weights.output_norm = llm_load_gguf_tensor_alias(llm_model.start, llm_model.size, llm_alias_output_norm, &matched_name);
    if (!llm_weights.output_norm) return 0;

    // Map all layers
    uint32_t n_layers = llm_model.gguf.arch.n_layer;
    if (n_layers > 32) n_layers = 32;

    for (uint32_t i = 0; i < n_layers; i++) {
        llama_build_layer_name(name, i, "attn_norm.weight");
        llm_weights.layers[i].attn_norm = tensor_load_gguf(llm_model.start, llm_model.size, name);
        
        llama_build_layer_name(name, i, "ffn_norm.weight");
        llm_weights.layers[i].ffn_norm = tensor_load_gguf(llm_model.start, llm_model.size, name);

        llama_build_layer_name(name, i, "ffn_gate.weight");
        llm_weights.layers[i].ffn_gate = tensor_load_gguf_view(llm_model.start, llm_model.size, name);

        llama_build_layer_name(name, i, "ffn_up.weight");
        llm_weights.layers[i].ffn_up = tensor_load_gguf_view(llm_model.start, llm_model.size, name);

        llama_build_layer_name(name, i, "ffn_down.weight");
        llm_weights.layers[i].ffn_down = tensor_load_gguf_view(llm_model.start, llm_model.size, name);

        // Load attention projection weights as lightweight VIEWS to save heap RAM
        llama_build_layer_name(name, i, "attn_q.weight");
        llm_weights.layers[i].attn_q = tensor_load_gguf_view(llm_model.start, llm_model.size, name);
        
        llama_build_layer_name(name, i, "attn_k.weight");
        llm_weights.layers[i].attn_k = tensor_load_gguf_view(llm_model.start, llm_model.size, name);

        llama_build_layer_name(name, i, "attn_v.weight");
        llm_weights.layers[i].attn_v = tensor_load_gguf_view(llm_model.start, llm_model.size, name);

        llama_build_layer_name(name, i, "attn_output.weight");
        llm_weights.layers[i].attn_o = tensor_load_gguf_view(llm_model.start, llm_model.size, name);

        if (i == 0 && llm_trace_on) {
            char tline[128];
            uint32_t tpos = 0;
            append_str(tline, &tpos, sizeof(tline), "LLM WEIGHT TYPES L0: q=");
            append_u32(tline, &tpos, sizeof(tline), llm_weights.layers[i].attn_q ? llm_weights.layers[i].attn_q->gguf_type : 99);
            append_str(tline, &tpos, sizeof(tline), " k=");
            append_u32(tline, &tpos, sizeof(tline), llm_weights.layers[i].attn_k ? llm_weights.layers[i].attn_k->gguf_type : 99);
            append_str(tline, &tpos, sizeof(tline), " v=");
            append_u32(tline, &tpos, sizeof(tline), llm_weights.layers[i].attn_v ? llm_weights.layers[i].attn_v->gguf_type : 99);
            append_str(tline, &tpos, sizeof(tline), " o=");
            append_u32(tline, &tpos, sizeof(tline), llm_weights.layers[i].attn_o ? llm_weights.layers[i].attn_o->gguf_type : 99);
            append_str(tline, &tpos, sizeof(tline), " (0=F32 1=F16 2=Q4_0)\n");
            klog(tline);
        }
    }

    return 1;
}

static void llm_release_llama_weights(void) {
    if (llm_weights.output_norm) {
        tensor_free(llm_weights.output_norm);
        llm_weights.output_norm = 0;
    }
    
    uint32_t n_layers = llm_model.gguf.arch.n_layer;
    if (n_layers > 32) n_layers = 32;

    for (uint32_t i = 0; i < n_layers; i++) {
        if (llm_weights.layers[i].attn_norm) { tensor_free(llm_weights.layers[i].attn_norm); llm_weights.layers[i].attn_norm = 0; }
        if (llm_weights.layers[i].ffn_norm) { tensor_free(llm_weights.layers[i].ffn_norm); llm_weights.layers[i].ffn_norm = 0; }
        if (llm_weights.layers[i].ffn_gate) { tensor_free(llm_weights.layers[i].ffn_gate); llm_weights.layers[i].ffn_gate = 0; }
        if (llm_weights.layers[i].ffn_up) { tensor_free(llm_weights.layers[i].ffn_up); llm_weights.layers[i].ffn_up = 0; }
        if (llm_weights.layers[i].ffn_down) { tensor_free(llm_weights.layers[i].ffn_down); llm_weights.layers[i].ffn_down = 0; }
        if (llm_weights.layers[i].attn_q) { tensor_free(llm_weights.layers[i].attn_q); llm_weights.layers[i].attn_q = 0; }
        if (llm_weights.layers[i].attn_k) { tensor_free(llm_weights.layers[i].attn_k); llm_weights.layers[i].attn_k = 0; }
        if (llm_weights.layers[i].attn_v) { tensor_free(llm_weights.layers[i].attn_v); llm_weights.layers[i].attn_v = 0; }
        if (llm_weights.layers[i].attn_o) { tensor_free(llm_weights.layers[i].attn_o); llm_weights.layers[i].attn_o = 0; }
    }
    
    llm_weights.token_embd = 0;
    llm_weights.output = 0;
    llm_weights_loaded = 0;
}

static int llm_read_gguf_tensor_alias_row(const char **aliases, const char *missing_name, uint32_t row_index, fixed_t *out, uint32_t out_count) {
    const char *matched_name = 0;

    if (!llm_find_gguf_tensor_alias(llm_model.start, llm_model.size, aliases, &matched_name)) {
        return llm_set_missing_runtime_tensor(missing_name);
    }
    if (!tensor_read_gguf_row_into(llm_model.start, llm_model.size, matched_name, row_index, out, out_count)) {
        return llm_set_runtime_tensor_error(matched_name);
    }
    return 1;
}

typedef struct {
    tensor_t *h;           // Current hidden state
    tensor_t *q;           // Query buffer
    tensor_t *k;           // Key buffer
    tensor_t *v;           // Value buffer
    tensor_t *att_scores;  // Attention scores (n_head x n_ctx)
    tensor_t *ffn_gate;    // FFN intermediate
    tensor_t *ffn_up;      // FFN intermediate
    tensor_t *scratch;     // Multi-purpose buffer
    // KV Cache per layer
    tensor_t *k_cache[32]; // Key cache per layer (max 32 layers)
    tensor_t *v_cache[32]; // Value cache per layer (max 32 layers)
    uint32_t current_pos;  // Current sequence token position
} llama_context_t;

static llama_context_t llm_ctx = {0};

#define LLM_MAX_CTX 64

static void llm_free_context(void) {
    if (llm_ctx.h) { tensor_free(llm_ctx.h); llm_ctx.h = 0; }
    if (llm_ctx.q) { tensor_free(llm_ctx.q); llm_ctx.q = 0; }
    if (llm_ctx.k) { tensor_free(llm_ctx.k); llm_ctx.k = 0; }
    if (llm_ctx.v) { tensor_free(llm_ctx.v); llm_ctx.v = 0; }
    if (llm_ctx.scratch) { tensor_free(llm_ctx.scratch); llm_ctx.scratch = 0; }
    if (llm_ctx.ffn_gate) { tensor_free(llm_ctx.ffn_gate); llm_ctx.ffn_gate = 0; }
    if (llm_ctx.ffn_up) { tensor_free(llm_ctx.ffn_up); llm_ctx.ffn_up = 0; }
    if (llm_ctx.att_scores) { tensor_free(llm_ctx.att_scores); llm_ctx.att_scores = 0; }

    for (uint32_t i = 0; i < 32; i++) {
        if (llm_ctx.k_cache[i]) { tensor_free(llm_ctx.k_cache[i]); llm_ctx.k_cache[i] = 0; }
        if (llm_ctx.v_cache[i]) { tensor_free(llm_ctx.v_cache[i]); llm_ctx.v_cache[i] = 0; }
    }
    llm_ctx.current_pos = 0;
}

/* Grouped-Query Attention support: when n_kv_head < n_head (e.g.
 * TinyLlama-1.1B: 32 query heads sharing 4 KV heads), attn_k/attn_v project
 * to a narrower n_kv_head*head_dim width than Q's n_embd, and groups of
 * n_head/n_kv_head adjacent query heads share one KV head. Metadata can be
 * missing or malformed (some GGUF files omit llama.attention.head_count_kv
 * entirely for plain multi-head models), so fall back to ordinary MHA
 * (n_kv_head = n_head) whenever the value isn't a clean divisor of n_head. */
static uint32_t llm_effective_n_kv_head(uint32_t n_head) {
    uint32_t n_kv_head = llm_model.gguf.arch.n_kv_head;
    if (n_kv_head == 0 || n_kv_head > n_head || n_head % n_kv_head != 0) {
        return n_head;
    }
    return n_kv_head;
}

/* Llama-family models are trained with a leading BOS token on every
 * sequence; without it the model starts from a state it never saw in
 * training, which measurably degrades coherence. Read the real ID from
 * GGUF metadata (SentencePiece/Llama convention is 1) with a safe fallback
 * for models that omit the key. */
static uint32_t llm_get_bos_token_id(void) {
    uint32_t bos = 1;
    gguf_get_metadata_value(llm_model.start, llm_model.size, "tokenizer.ggml.bos_token_id", GGUF_METADATA_UINT32, &bos);
    return bos;
}

/* Generation previously ran to max_new_tokens/timeout unconditionally,
 * ignoring whenever the model's own argmax pick was end-of-sequence --
 * forcing continued output past the point a correct decoder would stop.
 * Convention default is 2 (Llama/SentencePiece "</s>"). */
static uint32_t llm_get_eos_token_id(void) {
    uint32_t eos = 2;
    gguf_get_metadata_value(llm_model.start, llm_model.size, "tokenizer.ggml.eos_token_id", GGUF_METADATA_UINT32, &eos);
    return eos;
}

static void llm_init_context(uint32_t n_embd, uint32_t n_head) {
    uint32_t dims[2];
    uint32_t n_kv_head = llm_effective_n_kv_head(n_head);
    uint32_t head_dim = n_head ? n_embd / n_head : n_embd;
    uint32_t kv_dim = n_kv_head * head_dim;

    dims[0] = 1; dims[1] = n_embd;
    llm_ctx.h = tensor_create(2, dims);
    llm_ctx.q = tensor_create(2, dims);
    /* k/v stay n_embd-wide (not kv_dim) even though only the first kv_dim
     * elements are meaningful as the K/V projection -- llm_ctx.v in
     * particular is reused later in the same layer as the destination for
     * the n_embd-wide O-projection and FFN-down outputs, so shrinking it
     * to kv_dim would overflow those writes. */
    llm_ctx.k = tensor_create(2, dims);
    llm_ctx.v = tensor_create(2, dims);
    llm_ctx.scratch = tensor_create(2, dims);

    dims[0] = 1; dims[1] = n_embd * 4;
    llm_ctx.ffn_gate = tensor_create(2, dims);
    llm_ctx.ffn_up = tensor_create(2, dims);

    dims[0] = n_head; dims[1] = LLM_MAX_CTX;
    llm_ctx.att_scores = tensor_create(2, dims);

    // Initialize KV Cache per layer
    uint32_t n_layers = llm_model.gguf.arch.n_layer;
    if (n_layers > 32) n_layers = 32;

    uint32_t cache_dims[2];
    cache_dims[0] = LLM_MAX_CTX;
    cache_dims[1] = kv_dim; /* GQA-aware: narrower than n_embd when n_kv_head < n_head */

    for (uint32_t i = 0; i < n_layers; i++) {
        llm_ctx.k_cache[i] = tensor_create(2, cache_dims);
        llm_ctx.v_cache[i] = tensor_create(2, cache_dims);
    }
    llm_ctx.current_pos = 0;
}

/* Correct F16->F32 conversion, including subnormals (exp==0, mantissa!=0):
 * Q6_K/Q4_0 per-(super)block scale deltas are frequently subnormal-range
 * f16 values for real trained weights (very small but genuinely nonzero,
 * e.g. ~1e-5), not exact zero. A version of this used to short-circuit ANY
 * exp==0 case to 0.0f (a common "fast but wrong" simplification) -- that
 * silently zeroed out every dequantized value in any block whose scale
 * happened to be subnormal instead of normal, which for output.weight's
 * Q6_K blocks was frequent enough to blow up the final vocab logits to
 * nonsensical magnitudes (see ROADMAP.md, TinyLlama coherence
 * investigation: logits in the billions instead of the normal tens). */
static float f16_to_float_fast(uint16_t h) {
    union { uint32_t u; float f; } conv;
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x03FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x0400) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000 | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    conv.u = bits;
    return conv.f;
}

static float dot_product_q4_0_float(const uint8_t *block_row, const float *h_float, uint32_t n) {
    register float logit = 0.0f;
    uint32_t nb = n / 32;
    for (uint32_t b = 0; b < nb; b++) {
        const uint8_t *block = block_row + b * 18;
        uint16_t delta_f16 = *(uint16_t *)block;
        float delta = f16_to_float_fast(delta_f16);

        const uint8_t *qs = block + 2;
        const float *h_ptr = h_float + (b * 32);

        /* ggml Q4_0 packing: byte i holds elements i (low nibble) and i+16
         * (high nibble) -- see the matching fix/comment in
         * dequantize_q4_0() (kernel/tensor.c). This used to pair each
         * byte's two nibbles with ADJACENT input elements (h_ptr[2i],
         * h_ptr[2i+1]) instead of the correct i/i+16 split, silently
         * dot-producting every weight against the wrong input dimension. */
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t q_raw = qs[i];
            logit += delta * (float)((int)(q_raw & 0x0F) - 8) * h_ptr[i];
            logit += delta * (float)((int)(q_raw >> 4) - 8) * h_ptr[i + 16];
        }
    }
    return logit;
}

/* Fused Q6_K dot product, mirroring dot_product_q4_0_float's approach: do
 * the dequant and the multiply-accumulate in a single pass instead of
 * dequantizing a full row into a buffer first (what tensor_read_gguf_row_into
 * does). For TinyLlama-1.1B's output.weight (Q6_K, 32000 vocab rows), the
 * slow row-then-dot-product path was the dominant cost of generation --
 * every vocab candidate for every generated token went through it. Block
 * layout matches dequantize_q6_k() in tensor.c (210 bytes/256 elements). */
static float dot_product_q6_k_float(const uint8_t *block_row, const float *h_float, uint32_t n) {
    float logit = 0.0f;
    uint32_t nb = n / 256;
    for (uint32_t b = 0; b < nb; b++) {
        const uint8_t *block = block_row + b * 210;
        const uint8_t *ql = block;
        const uint8_t *qh = block + 128;
        const int8_t *sc = (const int8_t *)(block + 192);
        uint16_t d_f16 = *(const uint16_t *)(block + 208);
        float d = f16_to_float_fast(d_f16);
        const float *hp = h_float + (uint32_t)b * 256;

        for (int n2 = 0; n2 < 256; n2 += 128) {
            for (int l = 0; l < 32; l++) {
                int is = l / 16;
                int8_t q1 = (int8_t)((ql[l +  0] & 0x0F) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int8_t q2 = (int8_t)((ql[l + 32] & 0x0F) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int8_t q3 = (int8_t)((ql[l +  0] >>   4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int8_t q4 = (int8_t)((ql[l + 32] >>   4) | (((qh[l] >> 6) & 3) << 4)) - 32;

                logit += d * (float)sc[is + 0] * (float)q1 * hp[l +  0];
                logit += d * (float)sc[is + 2] * (float)q2 * hp[l + 32];
                logit += d * (float)sc[is + 4] * (float)q3 * hp[l + 64];
                logit += d * (float)sc[is + 6] * (float)q4 * hp[l + 96];
            }
            hp += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
    return logit;
}

/* Reused row-scratch buffer for llama_matmul_fpu()/the vocab argmax scan --
 * the hottest reads in the kernel (every Q/K/V/O/FFN projection, every
 * layer, every generated token). Reading through gguf_read_data_bytes()
 * instead of a raw pointer is what lets the model's tensor DATA section
 * live in high memory (kernel/llm_gguf.c); grown lazily and kept around
 * across calls instead of malloc/free per row. */
static uint8_t *llm_row_scratch = NULL;
static uint32_t llm_row_scratch_cap = 0;

static uint8_t *llm_row_scratch_ensure(uint32_t needed) {
    if (needed <= llm_row_scratch_cap) return llm_row_scratch;
    uint8_t *grown = kmalloc(needed);
    if (!grown) return NULL;
    if (llm_row_scratch) kfree(llm_row_scratch);
    llm_row_scratch = grown;
    llm_row_scratch_cap = needed;
    return llm_row_scratch;
}

/* weights->gguf_type previously didn't exist (tensor_t had no type field at
 * all), so this always treated every weight matrix as Q4_0 regardless of
 * its actual GGUF storage type. stories15M keeps attn_q.weight in a
 * different type than attn_k/attn_v -- misreading it as Q4_0 produced
 * garbage immediately after the very first Q projection (confirmed via the
 * "L0 q post-proj" energy probe exploding while K/V stayed sane), which
 * then poisoned every downstream computation regardless of prompt. */
static void llama_matmul_fpu(tensor_t *dest, tensor_t *src, tensor_t *weights, float *src_float) {
    if (!dest || !src || !weights || !src_float) return;

    uint32_t n_in = weights->dims[0];
    uint32_t n_out = (weights->ndims > 1) ? weights->dims[1] : weights->dims[0];
    uint32_t data_offset = llm_model.gguf.data_offset;

    if (weights->gguf_type == GGML_TYPE_Q4_0) {
        uint32_t blocks_per_row = n_in / 32;
        uint32_t row_bytes = blocks_per_row * 18;
        uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
        for (uint32_t i = 0; i < n_out; i++) {
            if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, data_offset, weights->gguf_offset + i * row_bytes, scratch, row_bytes)) {
                dest->data[i] = 0;
                continue;
            }
            float logit = dot_product_q4_0_float(scratch, src_float, n_in);
            dest->data[i] = float_to_fixed(logit);
        }
    } else if (weights->gguf_type == GGML_TYPE_Q6_K) {
        uint32_t blocks_per_row = n_in / 256;
        uint32_t row_bytes = blocks_per_row * 210;
        uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
        for (uint32_t i = 0; i < n_out; i++) {
            if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, data_offset, weights->gguf_offset + i * row_bytes, scratch, row_bytes)) {
                dest->data[i] = 0;
                continue;
            }
            float logit = dot_product_q6_k_float(scratch, src_float, n_in);
            dest->data[i] = float_to_fixed(logit);
        }
    } else if (weights->gguf_type == GGML_TYPE_F32) {
        uint32_t row_bytes = n_in * 4;
        uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
        for (uint32_t i = 0; i < n_out; i++) {
            if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, data_offset, weights->gguf_offset + (uint32_t)i * row_bytes, scratch, row_bytes)) {
                dest->data[i] = 0;
                continue;
            }
            float *row = (float *)scratch;
            float logit = 0.0f;
            for (uint32_t d = 0; d < n_in; d++) logit += row[d] * src_float[d];
            dest->data[i] = float_to_fixed(logit);
        }
    } else if (weights->gguf_type == GGML_TYPE_F16) {
        uint32_t row_bytes = n_in * 2;
        uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
        for (uint32_t i = 0; i < n_out; i++) {
            if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, data_offset, weights->gguf_offset + (uint32_t)i * row_bytes, scratch, row_bytes)) {
                dest->data[i] = 0;
                continue;
            }
            uint16_t *row = (uint16_t *)scratch;
            float logit = 0.0f;
            for (uint32_t d = 0; d < n_in; d++) logit += f16_to_float_fast(row[d]) * src_float[d];
            dest->data[i] = float_to_fixed(logit);
        }
    } else {
        /* Unknown/unsupported quant type: zero instead of misreading the
         * bytes as something they're not. */
        for (uint32_t i = 0; i < n_out; i++) dest->data[i] = 0;
    }
}

/* Forward declaration: defined later next to the llm_dump_top_logits
 * diagnostic, needed here too for llm_debug_h_energy below. */
static void append_float3(char *out, uint32_t *pos, uint32_t max, float value);

/* Debug aid for the still-open hidden-state explosion investigation: logs
 * sum(h[i]^2) computed directly in float (bypassing fixed_mul, whose
 * Q16.16 squaring silently overflows int32 for |x| > ~46340 -- exactly the
 * kind of value we're hunting for here) so the energy reading itself stays
 * trustworthy even once the bug it's tracking has corrupted other fixed_t
 * math. Gated behind llm_trace_on so normal `llm ask` runs aren't spammed. */
static void llm_debug_tensor_energy(const char *label, fixed_t *data, uint32_t count) {
    float sumsq;
    char line[96];
    uint32_t pos;

    if (!llm_trace_on) return;

    sumsq = 0.0f;
    for (uint32_t d = 0; d < count; d++) {
        float v = (float)data[d] / 65536.0f;
        sumsq += v * v;
    }

    pos = 0;
    append_str(line, &pos, sizeof(line), "LLM ENERGY ");
    append_str(line, &pos, sizeof(line), label);
    append_str(line, &pos, sizeof(line), " sumsq=");
    append_float3(line, &pos, sizeof(line), sumsq);
    append_str(line, &pos, sizeof(line), "\n");
    klog(line);
}

static void llm_debug_h_energy(const char *label, uint32_t n_embd) {
    float sumsq;
    char line[96];
    uint32_t pos;

    if (!llm_trace_on) return;

    sumsq = 0.0f;
    for (uint32_t d = 0; d < n_embd; d++) {
        float v = (float)llm_ctx.h->data[d] / 65536.0f;
        sumsq += v * v;
    }

    pos = 0;
    append_str(line, &pos, sizeof(line), "LLM ENERGY ");
    append_str(line, &pos, sizeof(line), label);
    append_str(line, &pos, sizeof(line), " h_sumsq=");
    append_float3(line, &pos, sizeof(line), sumsq);
    append_str(line, &pos, sizeof(line), "\n");
    klog(line);
}

/* float_to_fixed() truncates towards zero, so any eps below 1/65536
 * (~1.5e-5) in Q16.16 collapses to exactly 0 -- true for every typical
 * Llama rms_norm_eps (1e-5 or 1e-6), silently disabling RMSNorm's
 * numerical-stability term for every model. Round tiny-but-nonzero eps up
 * to the smallest representable positive value instead of losing it. */
static fixed_t llm_rms_eps_fixed(float eps) {
    fixed_t fx = float_to_fixed(eps);
    if (fx == 0 && eps > 0.0f) {
        fx = 1;
    }
    return fx;
}

static int run_llama_forward_pass(float *h_float, float *ffn_gate_float, uint32_t n_embd, uint32_t start_ticks) {
    uint32_t n_layers = llm_model.gguf.arch.n_layer;
    if (n_layers > 32) n_layers = 32;

    llm_debug_h_energy("pre-layers", n_embd);

    for (uint32_t layer = 0; layer < n_layers; layer++) {
        if (llm_generation_cancelled(start_ticks)) {
            return 0;
        }

        // --- 1. ATTENTION PASS ---
        if (llm_weights.layers[layer].attn_norm && llm_weights.layers[layer].attn_q) {
            if (!tensor_rmsnorm(llm_ctx.scratch, llm_ctx.h, llm_weights.layers[layer].attn_norm, llm_rms_eps_fixed(llm_model.gguf.arch.rms_norm_eps))) {
                klog("LLM error: attn_norm rmsnorm failed\n");
                return 0;
            }

            if (layer == 0) llm_debug_tensor_energy("L0 attn_norm rmsnorm-out", llm_ctx.scratch->data, n_embd);

            // Convert normalized state to float
            for (uint32_t d = 0; d < n_embd; d++) {
                h_float[d] = (float)llm_ctx.scratch->data[d] / 65536.0f;
            }

            // Rotary Positional Embeddings (RoPE) dims, needed before the K/V
            // projections below so the debug energy probes can use the
            // correct kv_dim width instead of n_embd (llm_ctx.k/v only hold
            // kv_dim meaningful elements before they're cached; the rest of
            // the buffer is stale data from a previous call).
            uint32_t n_head = llm_model.gguf.arch.n_head;
            uint32_t head_dim = n_embd / n_head;
            uint32_t n_kv_head = llm_effective_n_kv_head(n_head);
            uint32_t kv_dim = n_kv_head * head_dim;
            uint32_t group_size = n_head / n_kv_head; /* query heads sharing each KV head */

            // Project Q, K, V
            llama_matmul_fpu(llm_ctx.q, llm_ctx.scratch, llm_weights.layers[layer].attn_q, h_float);
            if (llm_generation_cancelled(start_ticks)) return 0;
            llama_matmul_fpu(llm_ctx.k, llm_ctx.scratch, llm_weights.layers[layer].attn_k, h_float);
            if (llm_generation_cancelled(start_ticks)) return 0;
            llama_matmul_fpu(llm_ctx.v, llm_ctx.scratch, llm_weights.layers[layer].attn_v, h_float);
            if (llm_generation_cancelled(start_ticks)) return 0;

            if (layer == 0) llm_debug_tensor_energy("L0 q post-proj", llm_ctx.q->data, n_embd);
            if (layer == 0) llm_debug_tensor_energy("L0 k post-proj", llm_ctx.k->data, kv_dim);
            if (layer == 0) llm_debug_tensor_energy("L0 v post-proj", llm_ctx.v->data, kv_dim);

            tensor_rope(llm_ctx.q, llm_ctx.current_pos, n_head, head_dim, llm_model.gguf.arch.rope_freq_base);
            tensor_rope(llm_ctx.k, llm_ctx.current_pos, n_kv_head, head_dim, llm_model.gguf.arch.rope_freq_base);

            if (layer == 0) llm_debug_tensor_energy("L0 q post-rope", llm_ctx.q->data, n_embd);

            // Store K & V in the KV Cache (kv_dim-wide: narrower than n_embd under GQA)
            if (llm_ctx.current_pos < LLM_MAX_CTX) {
                uint32_t cache_offset = llm_ctx.current_pos * kv_dim;
                for (uint32_t d = 0; d < kv_dim; d++) {
                    llm_ctx.k_cache[layer]->data[cache_offset + d] = llm_ctx.k->data[d];
                    llm_ctx.v_cache[layer]->data[cache_offset + d] = llm_ctx.v->data[d];
                }
            }

            // Compute Attention Scores & Mixed Values per Head
            for (uint32_t h_idx = 0; h_idx < n_head; h_idx++) {
                uint32_t kv_head = h_idx / group_size; /* GQA: map query head to its shared KV head */
                fixed_t *q_head = llm_ctx.q->data + h_idx * head_dim;
                fixed_t scale = fixed_sqrt(int_to_fixed(head_dim));
                if (scale == 0) scale = FIXED_ONE;

                for (uint32_t t_idx = 0; t_idx <= llm_ctx.current_pos && t_idx < LLM_MAX_CTX; t_idx++) {
                    fixed_t *k_head = llm_ctx.k_cache[layer]->data + t_idx * kv_dim + kv_head * head_dim;

                    fixed_t score = 0;
                    for (uint32_t d = 0; d < head_dim; d++) {
                        score += fixed_mul(q_head[d], k_head[d]);
                    }
                    score = fixed_div(score, scale);
                    llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx] = score;
                }

                // Softmax over sequence context length [0, current_pos]
                fixed_t max_val = llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + 0];
                for (uint32_t t_idx = 1; t_idx <= llm_ctx.current_pos && t_idx < LLM_MAX_CTX; t_idx++) {
                    fixed_t val = llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx];
                    if (val > max_val) max_val = val;
                }

                fixed_t sum_exp = 0;
                for (uint32_t t_idx = 0; t_idx <= llm_ctx.current_pos && t_idx < LLM_MAX_CTX; t_idx++) {
                    fixed_t exp_val = fixed_exp(llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx] - max_val);
                    llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx] = exp_val;
                    sum_exp += exp_val;
                }

                if (sum_exp == 0) sum_exp = FIXED_ONE;
                fixed_t inv_sum = fixed_div(FIXED_ONE, sum_exp);
                for (uint32_t t_idx = 0; t_idx <= llm_ctx.current_pos && t_idx < LLM_MAX_CTX; t_idx++) {
                    llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx] = fixed_mul(llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx], inv_sum);
                }
            }

            // Weighted sum of values (mix V cache)
            for (uint32_t d = 0; d < n_embd; d++) {
                llm_ctx.scratch->data[d] = 0;
            }

            for (uint32_t h_idx = 0; h_idx < n_head; h_idx++) {
                uint32_t kv_head = h_idx / group_size; /* GQA: map query head to its shared KV head */
                fixed_t *out_head = llm_ctx.scratch->data + h_idx * head_dim;
                for (uint32_t t_idx = 0; t_idx <= llm_ctx.current_pos && t_idx < LLM_MAX_CTX; t_idx++) {
                    fixed_t *v_head = llm_ctx.v_cache[layer]->data + t_idx * kv_dim + kv_head * head_dim;
                    fixed_t weight = llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx];
                    for (uint32_t d = 0; d < head_dim; d++) {
                        out_head[d] += fixed_mul(weight, v_head[d]);
                    }
                }
            }

            if (layer == 0) llm_debug_tensor_energy("L0 scratch post-vmix", llm_ctx.scratch->data, n_embd);

            // Project mixed attention output with O weights
            for (uint32_t d = 0; d < n_embd; d++) {
                h_float[d] = (float)llm_ctx.scratch->data[d] / 65536.0f;
            }
            
            if (llm_weights.layers[layer].attn_o) {
                llama_matmul_fpu(llm_ctx.v, llm_ctx.scratch, llm_weights.layers[layer].attn_o, h_float);
                if (llm_generation_cancelled(start_ticks)) return 0;
                if (layer == 0) llm_debug_tensor_energy("L0 v post-oproj", llm_ctx.v->data, n_embd);
                tensor_add(llm_ctx.h, llm_ctx.h, llm_ctx.v);
            }
            {
                char lbl[32];
                uint32_t lp = 0;
                append_str(lbl, &lp, sizeof(lbl), "after-attn L");
                append_u32(lbl, &lp, sizeof(lbl), layer);
                llm_debug_h_energy(lbl, n_embd);
            }
        }

        // --- 2. FEED-FORWARD PASS (SwiGLU FFN) ---
        if (llm_weights.layers[layer].ffn_norm && llm_weights.layers[layer].ffn_gate && 
            llm_weights.layers[layer].ffn_up && llm_weights.layers[layer].ffn_down) {
            
            // Normalize h into scratch (same pattern as attn_norm above).
            // Previously had arguments swapped: (h, scratch) normalised scratch
            // (undefined at this point) and overwrote h, destroying the residual.
            if (!tensor_rmsnorm(llm_ctx.scratch, llm_ctx.h, llm_weights.layers[layer].ffn_norm, llm_rms_eps_fixed(llm_model.gguf.arch.rms_norm_eps))) {
                klog("LLM error: ffn_norm rmsnorm failed\n");
                return 0;
            }

            for (uint32_t d = 0; d < n_embd; d++) {
                h_float[d] = (float)llm_ctx.scratch->data[d] / 65536.0f;
            }

            // Project gate and up
            llama_matmul_fpu(llm_ctx.ffn_gate, llm_ctx.scratch, llm_weights.layers[layer].ffn_gate, h_float);
            if (llm_generation_cancelled(start_ticks)) return 0;
            llama_matmul_fpu(llm_ctx.ffn_up, llm_ctx.scratch, llm_weights.layers[layer].ffn_up, h_float);
            if (llm_generation_cancelled(start_ticks)) return 0;

            // Apply SwiGLU: ffn_gate = SiLU(ffn_gate) * ffn_up
            // n_ff is the FFN intermediate width, i.e. the OUTPUT dimension
            // of ffn_gate.weight, not its input dimension. For TinyLlama
            // ffn_gate.weight has dims=(2048,5632) -- dims[0]=2048 is n_embd
            // (the input width), dims[1]=5632 is the real n_ff. Using
            // dims[0] silently truncated the SwiGLU/copy loops below to the
            // first 2048 of 5632 valid elements, so the down-projection
            // (which correctly expects all 5632) read uninitialized heap
            // memory for the remaining 3584 -- deterministic garbage tied to
            // kmalloc's reuse pattern, not a race, which is why it
            // reproduced identically across runs. Mirrors the same n_out
            // logic llama_matmul_fpu() already uses.
            uint32_t n_ff = (llm_weights.layers[layer].ffn_gate->ndims > 1)
                ? llm_weights.layers[layer].ffn_gate->dims[1]
                : llm_weights.layers[layer].ffn_gate->dims[0];
            /* ffn_gate_float/llm_ctx.ffn_gate/llm_ctx.ffn_up were all sized
             * for n_embd*4 elements by the caller; fail loudly instead of
             * silently overflowing if some future model's real n_ff (now
             * read correctly above) exceeds that assumption. */
            if (n_ff > n_embd * 4) {
                klog("LLM error: FFN intermediate width exceeds allocated buffer.\n");
                return 0;
            }
            tensor_apply_activation(llm_ctx.ffn_gate, ACT_SILU);
            for (uint32_t d = 0; d < n_ff; d++) {
                llm_ctx.ffn_gate->data[d] = fixed_mul(llm_ctx.ffn_gate->data[d], llm_ctx.ffn_up->data[d]);
            }

            // Project down back to n_embd: llm_ctx.v = matmul(ffn_gate, ffn_down)
            for (uint32_t d = 0; d < n_ff; d++) {
                ffn_gate_float[d] = (float)llm_ctx.ffn_gate->data[d] / 65536.0f;
            }

            llama_matmul_fpu(llm_ctx.v, llm_ctx.ffn_gate, llm_weights.layers[layer].ffn_down, ffn_gate_float);
            if (llm_generation_cancelled(start_ticks)) return 0;

            // Add residual connection
            tensor_add(llm_ctx.h, llm_ctx.h, llm_ctx.v);
            {
                char lbl[32];
                uint32_t lp = 0;
                append_str(lbl, &lp, sizeof(lbl), "after-ffn L");
                append_u32(lbl, &lp, sizeof(lbl), layer);
                llm_debug_h_energy(lbl, n_embd);
            }
        }
    }

    if (!tensor_rmsnorm(llm_ctx.h, llm_ctx.h, llm_weights.output_norm, llm_rms_eps_fixed(llm_model.gguf.arch.rms_norm_eps))) {
        klog("LLM error: final output_norm rmsnorm failed\n");
        return 0;
    }
    llm_debug_h_energy("post-output_norm", n_embd);
    return 1;
}

/* Computes a single vocab row's logit against the current hidden state,
 * using the same fast Q4_0/Q6_K fused dot-product paths as the main argmax
 * scan. Used to check EOS specifically, since EOS is deliberately excluded
 * from that scan (llm_preview_token_usable() rejects '<'-prefixed control
 * tokens so they never get printed as if they were generated words). Returns
 * -1e30f (guaranteed to lose any comparison) if the row can't be read, so a
 * transient read failure just means generation continues normally instead
 * of forcing a stop. */
static float llm_output_logit_for_token(const gguf_tensor_t *out_tensor_info,
                                         float *h_float, uint32_t n_embd, uint32_t v) {
    uint32_t data_offset = llm_model.gguf.data_offset;
    if (out_tensor_info->type == GGML_TYPE_Q4_0) {
        uint32_t blocks_per_row = n_embd / 32;
        uint32_t row_bytes = blocks_per_row * 18;
        uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
        if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, data_offset, out_tensor_info->offset + v * row_bytes, scratch, row_bytes)) {
            return -1e30f;
        }
        return dot_product_q4_0_float(scratch, h_float, n_embd);
    }
    if (out_tensor_info->type == GGML_TYPE_Q6_K) {
        uint32_t blocks_per_row = n_embd / 256;
        uint32_t row_bytes = blocks_per_row * 210;
        uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
        if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, data_offset, out_tensor_info->offset + v * row_bytes, scratch, row_bytes)) {
            return -1e30f;
        }
        return dot_product_q6_k_float(scratch, h_float, n_embd);
    }
    if (!llm_read_gguf_tensor_alias_row(llm_alias_output, "output", v, llm_ctx.scratch->data, n_embd)) {
        return -1e30f;
    }
    float logit = 0.0f;
    for (uint32_t d = 0; d < n_embd; d++) {
        logit += ((float)llm_ctx.h->data[d] / 65536.0f) * ((float)llm_ctx.scratch->data[d] / 65536.0f);
    }
    return logit;
}

static int llm_try_gguf_inference(const char *prompt, char *response) {
    uint32_t n_embd = llm_model.gguf.arch.n_embd;
    extern uint32_t timer_get_ticks();

    if (!llm_model.valid_gguf_model) return 0;

    if (!llm_tokenizer) {
        llm_tokenizer = tokenizer_create(llm_model.start, llm_model.size);
        if (!llm_tokenizer) {
            copy_cstr_response(response, "LLM: GGUF tokenizer unavailable.");
            return 1;
        }
    }

    llm_trace_prompt_tokens(prompt, llm_tokenizer);

    uint32_t tokens[32];
    int n_tokens = tokenizer_encode(llm_tokenizer, prompt, tokens, 32);
    if (n_tokens == 0) return 0;

    if (!llm_ctx.h) {
        llm_init_context(n_embd, llm_model.gguf.arch.n_head);
    }
    
    if (!llm_weights_loaded) {
        llm_release_llama_weights();
        if (!llm_load_llama_weights()) {
            if (llm_gguf_runtime_reason_buf[0]) {
                copy_cstr_response(response, llm_gguf_runtime_reason_buf);
            } else {
                copy_cstr_response(response, "LLM: GGUF weights could not be loaded for generation.");
            }
            return 1;
        }
        llm_weights_loaded = 1;
    }

    // PRE-ALLOCATE FPU buffers once for the entire generation
    float *h_float = kmalloc(n_embd * sizeof(float));
    if (!h_float) {
        copy_cstr_response(response, "LLM: Failed to allocate FPU buffer.");
        return 1;
    }

    uint32_t n_ff_max = n_embd * 4;
    float *ffn_gate_float = kmalloc(n_ff_max * sizeof(float));
    if (!ffn_gate_float) {
        copy_cstr_response(response, "LLM: Failed to allocate FPU buffer.");
        kfree(h_float);
        return 1;
    }

    uint32_t pos_out = 0;

    uint32_t last_tokens[16];
    for (int i = 0; i < 16; i++) last_tokens[i] = 0;

    uint32_t start_ticks = timer_get_ticks();

    // Reset sequence context position
    llm_ctx.current_pos = 0;

    // Every Llama-family sequence is trained with a leading BOS token; feed
    // it through as an extra position-0 step before the real prompt tokens
    // so the model starts from the state it actually saw in training.
    {
        uint32_t bos_token = llm_get_bos_token_id();
        if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", bos_token, llm_ctx.h->data, n_embd)) {
            copy_cstr_response(response, llm_gguf_runtime_reason_buf);
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }
        if (!run_llama_forward_pass(h_float, ffn_gate_float, n_embd, start_ticks)) {
            if (llm_generation_cancelled(start_ticks)) {
                llm_generation_cancel_response(response, start_ticks);
            } else {
                copy_cstr_response(response, "LLM: GGUF forward pass failed.");
            }
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }
        llm_ctx.current_pos++;
    }

    // Ingest all prompt tokens except the last one to populate the KV Cache
    for (int p_idx = 0; p_idx < n_tokens - 1; p_idx++) {
        uint32_t p_token = tokens[p_idx];
        if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", p_token, llm_ctx.h->data, n_embd)) {
            copy_cstr_response(response, llm_gguf_runtime_reason_buf);
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }
        if (!run_llama_forward_pass(h_float, ffn_gate_float, n_embd, start_ticks)) {
            if (llm_generation_cancelled(start_ticks)) {
                llm_generation_cancel_response(response, start_ticks);
            } else {
                copy_cstr_response(response, "LLM: GGUF forward pass failed.");
            }
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }
        llm_ctx.current_pos++;
    }

    uint32_t last_token = tokens[n_tokens - 1];
    int generated_count = 0;
    int max_new_tokens = llm_gguf_generation_token_limit();
    uint32_t eos_token_id = llm_get_eos_token_id();

    // Output tensor alias/info never changes during generation; resolve once.
    gguf_tensor_t out_tensor_info;
    const char *matched_output_name = 0;
    if (!llm_find_gguf_tensor_alias(llm_model.start, llm_model.size, llm_alias_output, &matched_output_name)) {
        copy_cstr_response(response, "LLM: output tensor alias not found.");
        kfree(ffn_gate_float);
        kfree(h_float);
        return 1;
    }
    if (!gguf_get_tensor(llm_model.start, llm_model.size, matched_output_name, &out_tensor_info)) {
        copy_cstr_response(response, "LLM: output tensor info not found.");
        kfree(ffn_gate_float);
        kfree(h_float);
        return 1;
    }

    while (generated_count < max_new_tokens) {
        if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", last_token, llm_ctx.h->data, n_embd)) {
            copy_cstr_response(response, llm_gguf_runtime_reason_buf);
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }

        if (!run_llama_forward_pass(h_float, ffn_gate_float, n_embd, start_ticks)) {
            if (llm_generation_cancelled(start_ticks)) {
                llm_generation_cancel_response(response, start_ticks);
            } else {
                copy_cstr_response(response, "LLM: GGUF forward pass failed.");
            }
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }

        uint32_t best_token = 0;

        // Convert current hidden state to float for fast projection
        for (uint32_t d = 0; d < n_embd; d++) {
            h_float[d] = (float)llm_ctx.h->data[d] / 65536.0f;
        }

        uint32_t vocab_limit = LLM_GGUF_PREVIEW_VOCAB_LIMIT;
        if (llm_model.gguf.arch.n_vocab < vocab_limit) vocab_limit = llm_model.gguf.arch.n_vocab;
        /* n_vocab is model metadata and can disagree with the output
         * tensor's own declared row count; never walk past the rows the
         * tensor actually has. */
        if (out_tensor_info.ndims >= 2 && out_tensor_info.dims[1] < vocab_limit) {
            vocab_limit = out_tensor_info.dims[1];
        }

        float max_logit_f = -1e30f;
        int found_token = 0;
        uint32_t heartbeat_step = vocab_limit / 8;
        if (heartbeat_step == 0) heartbeat_step = 1;
        for (uint32_t v = LLM_GGUF_FIRST_TEXT_TOKEN; v < vocab_limit; v++) {
            char candidate_buf[80];
            const char *candidate_raw = tokenizer_decode(llm_tokenizer, v);
            const char *candidate = llm_token_display(candidate_raw, candidate_buf, sizeof(candidate_buf));
            if (llm_generation_cancelled(start_ticks)) {
                llm_generation_cancel_response(response, start_ticks);
                kfree(ffn_gate_float);
                kfree(h_float);
                return 1;
            }

            if (v % heartbeat_step == 0 && v > 0) {
                klog("."); // Discrete progress heartbeat
            }
            if (!llm_preview_token_usable(candidate)) {
                continue;
            }

            float logit = 0.0f;
            // FAST PATH: Direct fused dot product using FLOAT math
            if (out_tensor_info.type == GGML_TYPE_Q4_0) {
                uint32_t blocks_per_row = n_embd / 32;
                uint32_t row_bytes = blocks_per_row * 18;
                uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
                if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, llm_model.gguf.data_offset, out_tensor_info.offset + v * row_bytes, scratch, row_bytes)) {
                    copy_cstr_response(response, "LLM: output tensor row read failed.");
                    kfree(ffn_gate_float);
                    kfree(h_float);
                    return 1;
                }
                logit = dot_product_q4_0_float(scratch, h_float, n_embd);
            } else if (out_tensor_info.type == GGML_TYPE_Q6_K) {
                uint32_t blocks_per_row = n_embd / 256;
                uint32_t row_bytes = blocks_per_row * 210;
                uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
                if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, llm_model.gguf.data_offset, out_tensor_info.offset + v * row_bytes, scratch, row_bytes)) {
                    copy_cstr_response(response, "LLM: output tensor row read failed.");
                    kfree(ffn_gate_float);
                    kfree(h_float);
                    return 1;
                }
                logit = dot_product_q6_k_float(scratch, h_float, n_embd);
                if (llm_trace_on && v < LLM_GGUF_FIRST_TEXT_TOKEN + 30) {
                    uint32_t chk = 0;
                    for (uint32_t bi = 0; bi < row_bytes; bi++) chk = chk * 31 + scratch[bi];
                    char dbg[128];
                    uint32_t dp = 0;
                    append_str(dbg, &dp, sizeof(dbg), "DBG v=");
                    append_u32(dbg, &dp, sizeof(dbg), v);
                    append_str(dbg, &dp, sizeof(dbg), " off=");
                    append_u32(dbg, &dp, sizeof(dbg), out_tensor_info.offset + v * row_bytes);
                    append_str(dbg, &dp, sizeof(dbg), " rb=");
                    append_u32(dbg, &dp, sizeof(dbg), row_bytes);
                    append_str(dbg, &dp, sizeof(dbg), " chk=");
                    append_u32(dbg, &dp, sizeof(dbg), chk);
                    append_str(dbg, &dp, sizeof(dbg), " logit=");
                    append_float3(dbg, &dp, sizeof(dbg), logit);
                    append_str(dbg, &dp, sizeof(dbg), "\n");
                    klog(dbg);
                }
            } else {
                // Fallback (slow)
                if (!llm_read_gguf_tensor_alias_row(llm_alias_output, "output", v, llm_ctx.scratch->data, n_embd)) {
                    copy_cstr_response(response, llm_gguf_runtime_reason_buf);
                    kfree(ffn_gate_float);
                    kfree(h_float);
                    return 1;
                }
                for (uint32_t d = 0; d < n_embd; d++) {
                    logit += ((float)llm_ctx.h->data[d] / 65536.0f) * ((float)llm_ctx.scratch->data[d] / 65536.0f);
                }
            }

            // REPETITION PENALTY: a flat -40 over a 16-token window effectively
            // banned ANY recently used token outright. Q4_0 dot-product logits
            // here are unnormalized and can be small in magnitude, so -40 was
            // large enough to permanently exclude common function words
            // ("a", "the", "to") that legitimately recur within a short
            // generation, forcing the decoder into rare/wrong continuations.
            // Use a much milder penalty over a short recency window instead.
            // last_tokens[] is a 16-slot ring buffer indexed by generated_count
            // % 16, so the actual N most recent tokens are at
            // (generated_count - 1) % 16, (generated_count - 2) % 16, ...
            for (int back = 1; back <= 4 && back <= generated_count; back++) {
                int idx = (generated_count - back) & 15;
                if (v == last_tokens[idx] && v != 0) {
                    logit -= 1.5f;
                }
            }

            if (logit > max_logit_f) {
                max_logit_f = logit;
                best_token = v;
                found_token = 1;
            }
        }

        // Compare the model's real end-of-sequence logit against the best
        // printable candidate; if the model actually prefers to stop here,
        // honor that instead of forcing further degenerate continuation.
        if (found_token) {
            float eos_logit = llm_output_logit_for_token(&out_tensor_info, h_float, n_embd, eos_token_id);
            if (eos_logit > max_logit_f) {
                if (llm_trace_on) {
                    trace_log_token_text("LLM TRACE OUT: ", eos_token_id, "<eos>");
                }
                break;
            }
        }

        if (!found_token || best_token < LLM_GGUF_FIRST_TEXT_TOKEN) {
            if (generated_count == 0) {
                char dbg[64];
                uint32_t dpos = 0;
                append_str(dbg, &dpos, 64, "LLM: Early exit (token=");
                append_u32(dbg, &dpos, 64, best_token);
                append_str(dbg, &dpos, 64, ").\n");
                klog(dbg);
            }
            if (llm_trace_on) {
                trace_log_token_text("LLM TRACE OUT: ", best_token, tokenizer_decode(llm_tokenizer, best_token));
            }
            break;
        }

        char word_buf[80];
        const char *word_raw = tokenizer_decode(llm_tokenizer, best_token);
        const char *word = llm_token_display(word_raw, word_buf, sizeof(word_buf));
        if (!word || !word[0]) {
            if (llm_trace_on) {
                trace_log_token_text("LLM TRACE OUT: ", best_token, "<empty>");
            }
            break;
        }
        if (llm_trace_on) {
            trace_log_token_text("LLM TRACE OUT: ", best_token, word);
        }
        
        // STREAMING: Print the word to the console immediately
        klog(word);
        
        /* response is always the shell's 128-byte buffer (see shell.c); this
         * must match copy_response()'s 127-char + NUL cap, not the tensor
         * row buffer sizes used elsewhere in this function. */
        append_str(response, &pos_out, 128, word);
        if (pos_out + 1 >= 128) {
            break; /* response buffer full; stop generating further tokens */
        }
        
        llm_ctx.current_pos++;
        last_tokens[generated_count % 16] = best_token;
        last_token = best_token;
        generated_count++;
    }
    
    kfree(ffn_gate_float);
    kfree(h_float);

    uint32_t end_ticks = timer_get_ticks();
    uint32_t total_ms = (end_ticks - start_ticks) * 10; // Assuming 100Hz

    if (generated_count == 0) {
        copy_cstr_response(response, "LLM: GGUF generative pass ran but produced no usable text tokens. Try a different prompt or check llm info.");
    }
    
    char report[128];
    uint32_t rpos = 0;
    append_str(report, &rpos, 128, "\n[LLM Report] Generated ");
    append_u32(report, &rpos, 128, generated_count);
    append_str(report, &rpos, 128, " tokens in ");
    append_u32(report, &rpos, 128, total_ms);
    append_str(report, &rpos, 128, " ms.\n");
    klog(report);

    return 1;
}

typedef struct {
    uint32_t token;
    float logit;
} llm_logit_entry_t;

static void llm_insert_top_n(llm_logit_entry_t *top, int *count, int max_n, uint32_t token, float logit) {
    int i;
    if (*count < max_n) {
        i = *count;
        (*count)++;
    } else if (logit > top[max_n - 1].logit) {
        i = max_n - 1;
    } else {
        return;
    }
    top[i].token = token;
    top[i].logit = logit;
    while (i > 0 && top[i - 1].logit < top[i].logit) {
        llm_logit_entry_t tmp = top[i - 1];
        top[i - 1] = top[i];
        top[i] = tmp;
        i--;
    }
}

static int float_is_finite(float value) {
    union { float f; uint32_t i; } v = { .f = value };
    return (v.i & 0x7F800000u) != 0x7F800000u;
}

static void append_float3(char *out, uint32_t *pos, uint32_t max, float value) {
    int neg;
    float av;

    if (!float_is_finite(value)) {
        append_str(out, pos, max, value < 0.0f ? "-inf/nan" : "inf/nan");
        return;
    }
    neg = value < 0.0f;
    av = neg ? -value : value;
    if (neg) append_str(out, pos, max, "-");

    /* Scaling by 1000 below for 3-decimal formatting would itself overflow
     * int32 for anything past ~2,000,000 (the exact failure mode that
     * earlier printed "-2147483.-648" instead of a legible huge number).
     * Past that point the fractional digits don't matter anyway -- print
     * the truncated integer part only, capped so the cast itself can't
     * overflow no matter how exploded the upstream value is. */
    if (av >= 2000000.0f) {
        if (av > 4000000000.0f) av = 4000000000.0f;
        append_u32(out, pos, max, (uint32_t)av);
        append_str(out, pos, max, "+");
        return;
    }

    {
        int32_t scaled = (int32_t)(av * 1000.0f + 0.5f);
        int32_t ip = scaled / 1000;
        int32_t fp = scaled % 1000;
        char fbuf[4];
        uint32_t flen;

        append_u32(out, pos, max, (uint32_t)ip);
        append_str(out, pos, max, ".");
        itoa(fp, fbuf, 10);
        flen = strlen(fbuf);
        for (uint32_t i = flen; i < 3; i++) append_str(out, pos, max, "0");
        append_str(out, pos, max, fbuf);
    }
}

/* Diagnostic aid for the LLM generation-quality bug hunt: runs the prefill
 * forward pass for `prompt` and, for the position right after the last
 * prompt token, dumps the hidden-state energy and the top-5 output logits
 * (token id, decoded text, raw logit) to the kernel log instead of doing
 * argmax + autoregressive generation. Lets the result be compared by hand
 * against a reference run (llama2.c run.c / a Python port) of the same
 * GGUF file and prompt, to localize exactly where the computation diverges
 * if the model keeps collapsing onto the same handful of tokens. */
static void llm_dump_top_logits(const char *prompt, char *response) {
    uint32_t n_embd = llm_model.gguf.arch.n_embd;
    extern uint32_t timer_get_ticks();
    float *h_float = NULL;
    float *ffn_gate_float = NULL;
    uint32_t n_ff_max;
    uint32_t tokens[32];
    int n_tokens;
    uint32_t start_ticks;
    uint32_t last_token;
    gguf_tensor_t out_tensor_info;
    const char *matched_output_name = 0;
    float h_sumsq;
    llm_logit_entry_t top[5];
    int top_count = 0;
    uint32_t vocab_limit;
    char line[160];
    uint32_t lpos;

    if (!llm_model.valid_gguf_model) {
        copy_cstr_response(response, "LLM: no GGUF model loaded.");
        return;
    }
    if (llm_backend() != LLM_BACKEND_GGUF_GEN) {
        copy_cstr_response(response, "LLM: backend is not GGUF-GENERATIVE.");
        return;
    }
    if (!llm_tokenizer) {
        llm_tokenizer = tokenizer_create(llm_model.start, llm_model.size);
        if (!llm_tokenizer) {
            copy_cstr_response(response, "LLM: GGUF tokenizer unavailable.");
            return;
        }
    }

    n_tokens = tokenizer_encode(llm_tokenizer, prompt, tokens, 32);
    if (n_tokens == 0) {
        copy_cstr_response(response, "LLM: prompt encoded to 0 tokens.");
        return;
    }

    if (!llm_ctx.h) {
        llm_init_context(n_embd, llm_model.gguf.arch.n_head);
    }
    if (!llm_weights_loaded) {
        llm_release_llama_weights();
        if (!llm_load_llama_weights()) {
            copy_cstr_response(response, llm_gguf_runtime_reason_buf[0] ? llm_gguf_runtime_reason_buf : "LLM: weights load failed.");
            return;
        }
        llm_weights_loaded = 1;
    }

    h_float = kmalloc(n_embd * sizeof(float));
    n_ff_max = n_embd * 4;
    ffn_gate_float = kmalloc(n_ff_max * sizeof(float));
    if (!h_float || !ffn_gate_float) {
        if (h_float) kfree(h_float);
        if (ffn_gate_float) kfree(ffn_gate_float);
        copy_cstr_response(response, "LLM: FPU buffer alloc failed.");
        return;
    }

    llm_ctx.current_pos = 0;
    start_ticks = timer_get_ticks();

    // Match llm_try_gguf_inference(): feed BOS through as position 0 before
    // the real prompt tokens, so this diagnostic reflects the same inputs
    // actual generation sees.
    {
        uint32_t bos_token = llm_get_bos_token_id();
        if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", bos_token, llm_ctx.h->data, n_embd) ||
            !run_llama_forward_pass(h_float, ffn_gate_float, n_embd, start_ticks)) {
            copy_cstr_response(response, "LLM: prefill forward pass failed.");
            kfree(ffn_gate_float);
            kfree(h_float);
            return;
        }
        llm_ctx.current_pos++;
    }

    for (int p_idx = 0; p_idx < n_tokens - 1; p_idx++) {
        uint32_t p_token = tokens[p_idx];
        if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", p_token, llm_ctx.h->data, n_embd) ||
            !run_llama_forward_pass(h_float, ffn_gate_float, n_embd, start_ticks)) {
            copy_cstr_response(response, "LLM: prefill forward pass failed.");
            kfree(ffn_gate_float);
            kfree(h_float);
            return;
        }
        llm_ctx.current_pos++;
    }

    last_token = tokens[n_tokens - 1];
    if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", last_token, llm_ctx.h->data, n_embd) ||
        !run_llama_forward_pass(h_float, ffn_gate_float, n_embd, start_ticks)) {
        copy_cstr_response(response, "LLM: forward pass failed.");
        kfree(ffn_gate_float);
        kfree(h_float);
        return;
    }

    if (!llm_find_gguf_tensor_alias(llm_model.start, llm_model.size, llm_alias_output, &matched_output_name) ||
        !gguf_get_tensor(llm_model.start, llm_model.size, matched_output_name, &out_tensor_info)) {
        copy_cstr_response(response, "LLM: output tensor not found.");
        kfree(ffn_gate_float);
        kfree(h_float);
        return;
    }
    h_sumsq = 0.0f;
    for (uint32_t d = 0; d < n_embd; d++) {
        h_float[d] = (float)llm_ctx.h->data[d] / 65536.0f;
        h_sumsq += h_float[d] * h_float[d];
    }

    lpos = 0;
    append_str(line, &lpos, sizeof(line), "LLM LOGITS: prompt_tokens=");
    append_u32(line, &lpos, sizeof(line), (uint32_t)n_tokens);
    append_str(line, &lpos, sizeof(line), " last_token=");
    append_u32(line, &lpos, sizeof(line), last_token);
    append_str(line, &lpos, sizeof(line), " h_sumsq=");
    append_float3(line, &lpos, sizeof(line), h_sumsq);
    append_str(line, &lpos, sizeof(line), "\n");
    klog(line);

    vocab_limit = llm_model.gguf.arch.n_vocab;
    /* Same as the generation loop: n_vocab metadata can claim more rows
     * than the output tensor actually has on disk. */
    if (out_tensor_info.ndims >= 2 && out_tensor_info.dims[1] < vocab_limit) {
        vocab_limit = out_tensor_info.dims[1];
    }
    for (uint32_t v = 0; v < vocab_limit; v++) {
        float logit;
        if (out_tensor_info.type == GGML_TYPE_Q4_0) {
            uint32_t blocks_per_row = n_embd / 32;
            uint32_t row_bytes = blocks_per_row * 18;
            uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
            if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, llm_model.gguf.data_offset, out_tensor_info.offset + v * row_bytes, scratch, row_bytes)) {
                continue;
            }
            logit = dot_product_q4_0_float(scratch, h_float, n_embd);
        } else if (out_tensor_info.type == GGML_TYPE_Q6_K) {
            uint32_t blocks_per_row = n_embd / 256;
            uint32_t row_bytes = blocks_per_row * 210;
            uint8_t *scratch = llm_row_scratch_ensure(row_bytes);
            if (!scratch || !gguf_read_data_bytes(llm_model.start, llm_model.size, llm_model.gguf.data_offset, out_tensor_info.offset + v * row_bytes, scratch, row_bytes)) {
                continue;
            }
            logit = dot_product_q6_k_float(scratch, h_float, n_embd);
            {
                /* LLM DIAG v1: extends the original v==0 differential check
                 * (fused dot_product_q6_k_float vs. dequantize_q6_k + manual
                 * dot product) to the actual offending rows from the
                 * "hello" top-5 (5489/8168/5917/5959/8232, see ROADMAP.md
                 * Etapa 10), plus row 0 as a known-sane control. Also dumps
                 * per-superblock raw d_f16/d/sc[] so a wrong-bytes-read bug
                 * (offset/stride) can be told apart from a pure-math bug by
                 * comparing against the same bytes read independently in
                 * Python at the identical offset formula
                 * (data_offset + tensor.offset + v*row_bytes). */
                static const uint32_t llm_diag_rows[] = {0, 5489, 8168, 5917, 5959, 8232, 26000};
                int is_diag_row = 0;
                for (uint32_t di = 0; di < sizeof(llm_diag_rows) / sizeof(llm_diag_rows[0]); di++) {
                    if (llm_diag_rows[di] == v) { is_diag_row = 1; break; }
                }
                if (is_diag_row && llm_trace_on) {
                    dequantize_q6_k(llm_ctx.scratch->data, scratch, n_embd);
                    float cross_logit = 0.0f;
                    for (uint32_t d = 0; d < n_embd; d++) {
                        cross_logit += h_float[d] * ((float)llm_ctx.scratch->data[d] / 65536.0f);
                    }
                    /* Address/offset trace: lets this be compared byte-for-byte
                     * against an independent Python read of the raw .gguf file
                     * at the same file-relative offset (data_offset +
                     * tensor.offset + v*row_bytes) to tell a wrong-bytes-read
                     * bug (offset/paging/PAE) apart from a pure-math bug. */
                    char dbg0[200];
                    uint32_t dp0 = 0;
                    append_str(dbg0, &dp0, sizeof(dbg0), "LLM DIAG v1 addr row=");
                    append_u32(dbg0, &dp0, sizeof(dbg0), v);
                    append_str(dbg0, &dp0, sizeof(dbg0), " model_start=");
                    append_u32(dbg0, &dp0, sizeof(dbg0), llm_model.start);
                    append_str(dbg0, &dp0, sizeof(dbg0), " data_offset=");
                    append_u32(dbg0, &dp0, sizeof(dbg0), llm_model.gguf.data_offset);
                    append_str(dbg0, &dp0, sizeof(dbg0), " tensor_offset=");
                    append_u32(dbg0, &dp0, sizeof(dbg0), out_tensor_info.offset);
                    append_str(dbg0, &dp0, sizeof(dbg0), " row_byte_off=");
                    append_u32(dbg0, &dp0, sizeof(dbg0), out_tensor_info.offset + v * row_bytes);
                    append_str(dbg0, &dp0, sizeof(dbg0), " phys_addr=");
                    append_u32(dbg0, &dp0, sizeof(dbg0), llm_model.start + llm_model.gguf.data_offset + out_tensor_info.offset + v * row_bytes);
                    append_str(dbg0, &dp0, sizeof(dbg0), " model_size=");
                    append_u32(dbg0, &dp0, sizeof(dbg0), llm_model.size);
                    append_str(dbg0, &dp0, sizeof(dbg0), "\n");
                    klog(dbg0);

                    /* Raw bytes 0..15 of the row (first block's d_f16 + first
                     * 14 ql bytes) as hex, one byte per token, for a direct
                     * byte-for-byte diff against Python's read of the same
                     * file-relative offset. */
                    char dbgraw[160];
                    uint32_t dpr = 0;
                    append_str(dbgraw, &dpr, sizeof(dbgraw), "LLM DIAG v1 rawbytes row=");
                    append_u32(dbgraw, &dpr, sizeof(dbgraw), v);
                    append_str(dbgraw, &dpr, sizeof(dbgraw), ":");
                    for (int rb = 0; rb < 16; rb++) {
                        char hex[4];
                        itoa((int)scratch[rb], hex, 16);
                        uint32_t hlen = 0;
                        while (hex[hlen]) hlen++;
                        append_str(dbgraw, &dpr, sizeof(dbgraw), " ");
                        if (hlen < 2) append_str(dbgraw, &dpr, sizeof(dbgraw), "0");
                        append_str(dbgraw, &dpr, sizeof(dbgraw), hex);
                    }
                    append_str(dbgraw, &dpr, sizeof(dbgraw), "\n");
                    klog(dbgraw);

                    char dbg[160];
                    uint32_t dpos = 0;
                    append_str(dbg, &dpos, sizeof(dbg), "LLM DIAG v1 row=");
                    append_u32(dbg, &dpos, sizeof(dbg), v);
                    append_str(dbg, &dpos, sizeof(dbg), " fused_logit=");
                    append_float3(dbg, &dpos, sizeof(dbg), logit);
                    append_str(dbg, &dpos, sizeof(dbg), " dequant_logit=");
                    append_float3(dbg, &dpos, sizeof(dbg), cross_logit);
                    append_str(dbg, &dpos, sizeof(dbg), "\n");
                    klog(dbg);

                    uint32_t blocks_per_row_diag = n_embd / 256;
                    for (uint32_t b = 0; b < blocks_per_row_diag; b++) {
                        const uint8_t *block = scratch + b * 210;
                        const int8_t *sc = (const int8_t *)(block + 192);
                        uint16_t d_f16b = *(const uint16_t *)(block + 208);
                        float db = f16_to_float_fast(d_f16b);
                        int8_t sc_min = sc[0], sc_max = sc[0];
                        for (int si = 1; si < 16; si++) {
                            if (sc[si] < sc_min) sc_min = sc[si];
                            if (sc[si] > sc_max) sc_max = sc[si];
                        }
                        char dbg2[160];
                        uint32_t dp2 = 0;
                        append_str(dbg2, &dp2, sizeof(dbg2), "  blk=");
                        append_u32(dbg2, &dp2, sizeof(dbg2), b);
                        append_str(dbg2, &dp2, sizeof(dbg2), " d_f16=0x");
                        {
                            char hex[8];
                            itoa((int)d_f16b, hex, 16);
                            uint32_t hlen = 0;
                            while (hex[hlen]) hlen++;
                            for (uint32_t pad = hlen; pad < 4; pad++) append_str(dbg2, &dp2, sizeof(dbg2), "0");
                            append_str(dbg2, &dp2, sizeof(dbg2), hex);
                        }
                        append_str(dbg2, &dp2, sizeof(dbg2), " d=");
                        append_float3(dbg2, &dp2, sizeof(dbg2), db);
                        append_str(dbg2, &dp2, sizeof(dbg2), " sc_min=");
                        append_u32(dbg2, &dp2, sizeof(dbg2), (uint32_t)(int32_t)sc_min);
                        append_str(dbg2, &dp2, sizeof(dbg2), " sc_max=");
                        append_u32(dbg2, &dp2, sizeof(dbg2), (uint32_t)(int32_t)sc_max);
                        append_str(dbg2, &dp2, sizeof(dbg2), "\n");
                        klog(dbg2);
                    }
                }
            }
        } else {
            if (!llm_read_gguf_tensor_alias_row(llm_alias_output, "output", v, llm_ctx.scratch->data, n_embd)) {
                continue;
            }
            logit = 0.0f;
            for (uint32_t d = 0; d < n_embd; d++) {
                logit += h_float[d] * ((float)llm_ctx.scratch->data[d] / 65536.0f);
            }
        }
        llm_insert_top_n(top, &top_count, 5, v, logit);
    }

    for (int i = 0; i < top_count; i++) {
        char tbuf[80];
        const char *disp = llm_token_display(tokenizer_decode(llm_tokenizer, top[i].token), tbuf, sizeof(tbuf));
        char out[160];
        uint32_t opos = 0;
        append_str(out, &opos, sizeof(out), "  #");
        append_u32(out, &opos, sizeof(out), (uint32_t)(i + 1));
        append_str(out, &opos, sizeof(out), " token=");
        append_u32(out, &opos, sizeof(out), top[i].token);
        append_str(out, &opos, sizeof(out), " logit=");
        append_float3(out, &opos, sizeof(out), top[i].logit);
        append_str(out, &opos, sizeof(out), " logit_millions=");
        append_float3(out, &opos, sizeof(out), top[i].logit / 1000000.0f);
        append_str(out, &opos, sizeof(out), " text=\"");
        append_str(out, &opos, sizeof(out), disp);
        append_str(out, &opos, sizeof(out), "\"\n");
        klog(out);
    }

    kfree(ffn_gate_float);
    kfree(h_float);
    copy_cstr_response(response, "LLM: top-5 logits dumped to kernel log above.");
}

void llm_dump_logits(const char *prompt, char *response) {
    llm_dump_top_logits(prompt, response);
}

/* llm_gguf_selftest: verify the GGUF generative backend actually produces tokens.
 * Returns: 1=PASS (tokens generated), -1=FAIL (0 tokens), 0=SKIP (not GGUF-GEN). */
int llm_gguf_selftest(void) {
    char response[256];

    if (llm_backend() != LLM_BACKEND_GGUF_GEN) {
        return 0;
    }

    memset(response, 0, sizeof(response));
    llm_inference("the", response);

    /* Pass if response is non-empty and does not start with "LLM:" (error prefix) */
    if (response[0] && response[0] != 'L') {
        return 1;
    }
    if (response[0] == 'L' && response[1] == 'L' && response[2] == 'M' && response[3] == ':') {
        return -1;
    }
    return response[0] ? 1 : -1;
}

void llm_server_init() {
}

/**
 * @brief Loads an LLM model from a memory module.
 * Detects the model format (MKLM or GGUF) and initializes the appropriate backend.
 */
void llm_load_model_module(uint32_t start, uint32_t end, const char *name) {
    if (end <= start) return;
    
    // Reserve the region in PMM so the kernel doesn't use it for page tables/allocations
    pmm_reserve_region(start, end - start);
    llm_release_llama_weights();
    llm_free_context();
    gguf_probe_invalidate();
    if (llm_model.loaded) {
        gguf_release_high_mem_backing(llm_model.start);
    }

    llm_model.start = start;
    llm_model.end = end;
    llm_model.size = end - start;
    memset(llm_name_buf, 0, sizeof(llm_name_buf));
    llm_set_gguf_reason("");
    if (name) {
        strncpy(llm_name_buf, name, sizeof(llm_name_buf) - 1);
        llm_model.name = llm_name_buf;
    } else {
        llm_model.name = "multiboot model";
    }
    llm_model.header = (microk_model_header_t *)start;
    llm_model.loaded = 1;
    llm_model.valid_microk_model = llm_validate_microk_model(start, llm_model.size);
    llm_model.valid_gguf_model = gguf_probe(start, llm_model.size, &llm_model.gguf);
    llm_model.gguf_gen_ready = llm_model.valid_gguf_model ? llm_gguf_generation_ready(start, llm_model.size) : 0;
    if (!vmm_protect_range(start, llm_model.size, 0x01)) {
        klog("LLM load: warning: could not mark model pages read-only.\n");
    }
    if (llm_model.valid_gguf_model) {
        gguf_migrate_data_to_high_mem(start, llm_model.size, llm_model.gguf.data_offset);
    }

    if (llm_tokenizer) {
        tokenizer_free(llm_tokenizer);
        llm_tokenizer = NULL;
    }
}

int llm_load_model_file(const char *name) {
    extern uint32_t fat32_get_size(const char *filename);
    extern int fat32_load_file(const char *filename, uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size);
    extern int fat32_get_entry_by_index(const char *path, uint32_t index, char *out_name, int *out_is_dir);

    char resolved_name[64];
    char entry_name[64];
    uint32_t size;
    uint32_t out_size = 0;
    void *load_addr;
    int matches = 0;

    if (!name) {
        return 0;
    }

    memset(resolved_name, 0, sizeof(resolved_name));
    strncpy(resolved_name, name, sizeof(resolved_name) - 1);

    size = fat32_get_size(resolved_name);
    if (size == 0) {
        for (uint32_t i = 0; i < 128; i++) {
            int is_dir = 0;
            memset(entry_name, 0, sizeof(entry_name));
            if (!fat32_get_entry_by_index("/", i, entry_name, &is_dir)) {
                break;
            }
            if (is_dir) {
                continue;
            }
            if (starts_with_ci(entry_name, name)) {
                strncpy(resolved_name, entry_name, sizeof(resolved_name) - 1);
                matches++;
                if (matches > 1) {
                    klog("LLM load: ambiguous file prefix.\n");
                    return 0;
                }
            }
        }
        if (matches == 1) {
            size = fat32_get_size(resolved_name);
        }
    }

    if (size == 0) {
        klog("LLM load: file not found.\n");
        return 0;
    }

    pmm_stats_t pmm_stats;
    memset(&pmm_stats, 0, sizeof(pmm_stats));
    pmm_get_stats(&pmm_stats);
    uint32_t free_bytes = pmm_stats.free_blocks * pmm_stats.page_size;
    uint32_t largest_free = pmm_largest_free_region_bytes();

    klog("LLM load: memory check required_kib=");
    log_u32((size + 1023) / 1024);
    klog(" free_kib=");
    log_u32(free_bytes / 1024);
    klog(" largest_contiguous_kib=");
    log_u32(largest_free / 1024);
    klog(" high_free_kib=");
    log_u32(pmm_stats.high_pool_free_blocks * (pmm_stats.page_size / 1024));
    klog(pmm_stats.high_pool_allocatable ? " high_allocatable=yes\n" : " high_allocatable=no\n");

    if (size > free_bytes || size > largest_free) {
        klog("LLM load: insufficient contiguous low memory for model buffer.\n");
        klog("LLM load: increase QEMU -m below 4GB, load a smaller model, or move model buffers to high memory after PAE support is complete.\n");
        return 0;
    }

    klog("LLM load: allocating buffer...");
    load_addr = pmm_alloc_region(size);
    if (!load_addr) {
        klog("LLM load: allocation failed.\n");
        return 0;
    }

    klog("LLM load: reading file...");
    if (!fat32_load_file(resolved_name, (uint8_t *)load_addr, size, &out_size) || out_size != size) {
        klog("LLM load: disk read failed.\n");
        return 0;
    }

    klog("LLM load: probing model...");
    llm_load_model_module((uint32_t)load_addr, (uint32_t)load_addr + size, resolved_name);
    if (llm_model.valid_gguf_model && llm_gguf_reason_buf[0]) {
        klog(" LLM load: GGUF detail: ");
        klog(llm_gguf_reason_buf);
        klog("\n");
    }
    klog("LLM load: probe finished.");
    return llm_model_supported();
}

int llm_model_loaded() {
    return llm_model.loaded;
}

int llm_model_valid() {
    return llm_model.loaded && llm_model.valid_microk_model;
}

int llm_model_supported() {
    return llm_model.loaded && (llm_model.valid_microk_model || llm_model.valid_gguf_model);
}

void llm_set_trace(int enabled) {
    llm_trace_on = enabled ? 1 : 0;
}

int llm_trace_enabled(void) {
    return llm_trace_on;
}

uint32_t llm_model_size() {
    return llm_model.size;
}

const char *llm_model_name() {
    return llm_model.loaded ? llm_model.name : "none";
}

const char *llm_status_string() {
    switch (llm_backend()) {
        case LLM_BACKEND_NONE:
            return "LLM: no model module loaded. Add a GRUB module named model/llm/gguf.\n";
        case LLM_BACKEND_MKRP:
            return "LLM: valid MicroK MKLM v1 rule model loaded. MKRP backend active.\n";
        case LLM_BACKEND_MKNN:
            return "LLM: valid MicroK MKLM v1 neural model loaded. MKNN backend active.\n";
        case LLM_BACKEND_GGUF_PARSER:
            return "LLM: GGUF model detected. Status: PARSER-ONLY. Generative pass pending.\n";
        case LLM_BACKEND_GGUF_GEN:
            return "LLM: GGUF model loaded. Status: GENERATIVE. Fluid text generation active with KV Cache.\n";
        case LLM_BACKEND_UNKNOWN:
        default:
            return "LLM: module loaded, but format is unsupported. Mock fallback active.\n";
    }
}

const char *llm_info_string() {
    uint32_t pos = 0;
    microk_nn_payload_t *nn;
    microk_rule_payload_t *rules;

    memset(llm_info_buf, 0, sizeof(llm_info_buf));

    switch (llm_backend()) {
        case LLM_BACKEND_MKNN:
            nn = llm_nn_payload();
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "LLM info: format=MKLM v1 backend=MKNN name=");
            append_model_name(llm_info_buf, &pos, sizeof(llm_info_buf));
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " size=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.size);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " bytes vocab=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), nn ? nn->vocab_count : 0);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " classes=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), nn ? nn->class_count : 0);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "\n");
            return llm_info_buf;
        case LLM_BACKEND_MKRP:
            rules = llm_rule_payload();
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "LLM info: format=MKLM v1 backend=MKRP name=");
            append_model_name(llm_info_buf, &pos, sizeof(llm_info_buf));
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " size=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.size);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " bytes rules=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), rules ? rules->entry_count : 0);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "\n");
            return llm_info_buf;
        case LLM_BACKEND_GGUF_PARSER:
        case LLM_BACKEND_GGUF_GEN:
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "LLM info: format=GGUF status=");
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), llm_backend() == LLM_BACKEND_GGUF_GEN ? "GENERATIVE" : "PARSER-ONLY");
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " name=");
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.gguf.arch.name[0] ? llm_model.gguf.arch.name : "unknown");
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "\n  arch: Llama-like (");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.gguf.arch.n_layer);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "L, ");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.gguf.arch.n_embd);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "D, ");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.gguf.arch.n_head);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "H) ctx=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.gguf.arch.n_ctx);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " vocab=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.gguf.arch.n_vocab);
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), " tensors=");
            append_u32(llm_info_buf, &pos, sizeof(llm_info_buf), llm_model.gguf.tensor_count);
            if (llm_gguf_reason_buf[0]) {
                append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "\n  detail: ");
                append_str(llm_info_buf, &pos, sizeof(llm_info_buf), llm_gguf_reason_buf);
            }
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), "\n");
            return llm_info_buf;
        case LLM_BACKEND_NONE:
            return "LLM info: no model loaded.\n";
        case LLM_BACKEND_UNKNOWN:
        default:
            return "LLM info: unsupported model module loaded.\n";
    }
}

const char *llm_test_string() {
    switch (llm_backend()) {
        case LLM_BACKEND_MKNN:
            return "LLM test prompts: hola | estado | kernel | ayuda | que estado tiene el kernel\n";
        case LLM_BACKEND_MKRP:
            return "LLM test prompts depend on the MKRP rule pairs in the loaded model.\n";
        case LLM_BACKEND_GGUF_PARSER:
        case LLM_BACKEND_GGUF_GEN:
            return "LLM test prompt: !<p> (example: !hola)\n";
        case LLM_BACKEND_NONE:
            return "LLM test unavailable: no model loaded.\n";
        case LLM_BACKEND_UNKNOWN:
        default:
            return "LLM test unavailable: unsupported model format.\n";
    }
}

#include "io.h"
#include "speaker.h"

// Mock inference engine with system control
void llm_inference(const char *prompt, char *response) {
    if (!llm_model.loaded) {
        copy_cstr_response(response, "LLM: no model loaded. Boot with a model module first.");
    } else if (llm_backend() == LLM_BACKEND_GGUF_GEN) {
        if (llm_try_gguf_inference(prompt, response)) {
            return;
        }
        return;
    } else if (llm_backend() == LLM_BACKEND_GGUF_PARSER) {
        if (!llm_tokenizer) {
            llm_tokenizer = tokenizer_create(llm_model.start, llm_model.size);
        }
        llm_trace_prompt_tokens(prompt, llm_tokenizer);
        if (llm_trace_on) {
            klog("LLM TRACE OUT: no generated tokens (GGUF parser-only).\n");
        }
        copy_cstr_response(response, "LLM: GGUF loaded, but generative inference is not implemented yet.");
        return;
    } else if (llm_try_nn_inference(prompt, response)) {
        return;
    } else if (llm_try_rule_inference(prompt, response)) {
        return;
    } else if (!llm_model.valid_microk_model && strstr(prompt, "model")) {
        copy_cstr_response(response, "LLM: module is loaded but not a valid MKLM v1 model.");
    } else if (strstr(prompt, "reboot")) {
        copy_cstr_response(response, "LLM: Reboot command denied from prompt context.");
        speaker_beep(1000, 50); // Warning beep
    } else if (strstr(prompt, "metrics")) {
        copy_cstr_response(response, "LLM: Accessing kernel metrics... System is running at 100Hz.");
        speaker_beep(750, 10); // Informational beep
    } else if (strstr(prompt, "status")) {
        switch (llm_backend()) {
            case LLM_BACKEND_MKNN:
                copy_cstr_response(response, "LLM: MKNN neural backend active. Inference is numeric and model-driven.");
                break;
            case LLM_BACKEND_MKRP:
                copy_cstr_response(response, "LLM: MKRP rule backend active. Responses are loaded from the model.");
                break;
            case LLM_BACKEND_GGUF_GEN:
                copy_cstr_response(response, "LLM: GGUF backend active. Generative inference engine is online.");
                break;
            case LLM_BACKEND_GGUF_PARSER:
                copy_cstr_response(response, "LLM: GGUF backend active, but generative inference is not implemented yet.");
                break;
            default:
                copy_cstr_response(response, "LLM: Model module loaded. Mock fallback active.");
                break;
        }
        speaker_beep(500, 10);
    } else if (strstr(prompt, "beep")) {
        copy_cstr_response(response, "LLM: Generating test sound...");
        speaker_beep(440, 20); // A4 note
    } else {
        copy_cstr_response(response, "LLM: Request processed. No system action required.");
    }
}

void llm_server_process() {
}

void sys_llm_query(const char *prompt, char *response) {
    llm_inference(prompt, response);
}

void llm_server_task() {
    // This server would wait for IPC messages in a real implementation
    while(1) {
        // Placeholder for inference processing
    }
}
