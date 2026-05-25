#include "llm.h"
#include "llm_gguf.h"
#include "tensor.h"
#include "tokenizer.h"
#include "pmm.h"
#include "video.h"
#include "kheap.h"
#include <string.h>

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
#define LLM_GGUF_PREVIEW_MAX_TOKENS 16
#define LLM_GGUF_PREVIEW_VOCAB_LIMIT 4096
#define LLM_GGUF_PREVIEW_TIMEOUT_TICKS 300
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
    if (!has_visible || visible_count < 3) {
        return 0;
    }
    return 1;
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
        trace_log_token_text("LLM TRACE IN: ", tokens[i], tokenizer_decode(tokenizer, tokens[i]));
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

static void llm_init_context(uint32_t n_embd, uint32_t n_head) {
    uint32_t dims[2];
    
    dims[0] = 1; dims[1] = n_embd;
    llm_ctx.h = tensor_create(2, dims);
    llm_ctx.q = tensor_create(2, dims);
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
    cache_dims[1] = n_embd;

    for (uint32_t i = 0; i < n_layers; i++) {
        llm_ctx.k_cache[i] = tensor_create(2, cache_dims);
        llm_ctx.v_cache[i] = tensor_create(2, cache_dims);
    }
    llm_ctx.current_pos = 0;
}

static float dot_product_q4_0_float(const uint8_t *block_row, const float *h_float, uint32_t n) {
    register float logit = 0.0f;
    uint32_t nb = n / 32;
    for (uint32_t b = 0; b < nb; b++) {
        const uint8_t *block = block_row + b * 18;
        uint16_t delta_f16 = *(uint16_t *)block;
        
        // Fast F16 to F32
        union { uint32_t u; float f; } conv;
        uint32_t sign = (delta_f16 & 0x8000) << 16;
        uint32_t exp = (delta_f16 >> 10) & 0x1F;
        uint32_t mant = delta_f16 & 0x03FF;
        float delta;
        if (exp == 0) {
            delta = 0.0f;
        } else {
            conv.u = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
            delta = conv.f;
        }

        const uint8_t *qs = block + 2;
        const float *h_ptr = h_float + (b * 32);
        
        // Manual unrolling for 32 elements per block
        // We process 4 elements at a time to keep the FPU pipeline full
        for (uint32_t i = 0; i < 16; i += 2) {
            uint8_t q_raw = qs[i];
            logit += delta * (float)((int)(q_raw & 0x0F) - 8) * h_ptr[i*2+0];
            logit += delta * (float)((int)(q_raw >> 4) - 8) * h_ptr[i*2+1];
            
            q_raw = qs[i+1];
            logit += delta * (float)((int)(q_raw & 0x0F) - 8) * h_ptr[i*2+2];
            logit += delta * (float)((int)(q_raw >> 4) - 8) * h_ptr[i*2+3];
        }
    }
    return logit;
}

static void llama_matmul_fpu(tensor_t *dest, tensor_t *src, tensor_t *weights, float *src_float) {
    if (!dest || !src || !weights || !src_float) return;
    
    uint32_t n_in = weights->dims[0];
    uint32_t n_out = (weights->ndims > 1) ? weights->dims[1] : weights->dims[0];
    uint8_t *weight_data = (uint8_t *)(llm_model.start + llm_model.gguf.data_offset + weights->gguf_offset);

    for (uint32_t i = 0; i < n_out; i++) {
        uint32_t blocks_per_row = n_in / 32;
        uint8_t *row_ptr = weight_data + (i * blocks_per_row * 18);
        float logit = dot_product_q4_0_float(row_ptr, src_float, n_in);
        dest->data[i] = float_to_fixed(logit);
    }
}

static int run_llama_forward_pass(float *h_float, float *ffn_gate_float, uint32_t n_embd) {
    uint32_t n_layers = llm_model.gguf.arch.n_layer;
    if (n_layers > 32) n_layers = 32;

    for (uint32_t layer = 0; layer < n_layers; layer++) {
        // --- 1. ATTENTION PASS ---
        if (llm_weights.layers[layer].attn_norm && llm_weights.layers[layer].attn_q) {
            if (!tensor_rmsnorm(llm_ctx.scratch, llm_ctx.h, llm_weights.layers[layer].attn_norm, float_to_fixed(llm_model.gguf.arch.rms_norm_eps))) {
                klog("LLM error: attn_norm rmsnorm failed\n");
                return 0;
            }
            
            // Convert normalized state to float
            for (uint32_t d = 0; d < n_embd; d++) {
                h_float[d] = (float)llm_ctx.scratch->data[d] / 65536.0f;
            }

            // Project Q, K, V
            llama_matmul_fpu(llm_ctx.q, llm_ctx.scratch, llm_weights.layers[layer].attn_q, h_float);
            llama_matmul_fpu(llm_ctx.k, llm_ctx.scratch, llm_weights.layers[layer].attn_k, h_float);
            llama_matmul_fpu(llm_ctx.v, llm_ctx.scratch, llm_weights.layers[layer].attn_v, h_float);

            // Rotary Positional Embeddings (RoPE)
            uint32_t n_head = llm_model.gguf.arch.n_head;
            uint32_t head_dim = n_embd / n_head;
            tensor_rope(llm_ctx.q, llm_ctx.current_pos, n_head, head_dim);
            tensor_rope(llm_ctx.k, llm_ctx.current_pos, n_head, head_dim);

            // Store K & V in the KV Cache
            if (llm_ctx.current_pos < LLM_MAX_CTX) {
                uint32_t cache_offset = llm_ctx.current_pos * n_embd;
                for (uint32_t d = 0; d < n_embd; d++) {
                    llm_ctx.k_cache[layer]->data[cache_offset + d] = llm_ctx.k->data[d];
                    llm_ctx.v_cache[layer]->data[cache_offset + d] = llm_ctx.v->data[d];
                }
            }

            // Compute Attention Scores & Mixed Values per Head
            for (uint32_t h_idx = 0; h_idx < n_head; h_idx++) {
                fixed_t *q_head = llm_ctx.q->data + h_idx * head_dim;
                fixed_t scale = fixed_sqrt(int_to_fixed(head_dim));
                if (scale == 0) scale = FIXED_ONE;

                for (uint32_t t_idx = 0; t_idx <= llm_ctx.current_pos && t_idx < LLM_MAX_CTX; t_idx++) {
                    fixed_t *k_head = llm_ctx.k_cache[layer]->data + t_idx * n_embd + h_idx * head_dim;
                    
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
                fixed_t *out_head = llm_ctx.scratch->data + h_idx * head_dim;
                for (uint32_t t_idx = 0; t_idx <= llm_ctx.current_pos && t_idx < LLM_MAX_CTX; t_idx++) {
                    fixed_t *v_head = llm_ctx.v_cache[layer]->data + t_idx * n_embd + h_idx * head_dim;
                    fixed_t weight = llm_ctx.att_scores->data[h_idx * LLM_MAX_CTX + t_idx];
                    for (uint32_t d = 0; d < head_dim; d++) {
                        out_head[d] += fixed_mul(weight, v_head[d]);
                    }
                }
            }

            // Project mixed attention output with O weights
            for (uint32_t d = 0; d < n_embd; d++) {
                h_float[d] = (float)llm_ctx.scratch->data[d] / 65536.0f;
            }
            
            if (llm_weights.layers[layer].attn_o) {
                llama_matmul_fpu(llm_ctx.v, llm_ctx.scratch, llm_weights.layers[layer].attn_o, h_float);
                tensor_add(llm_ctx.h, llm_ctx.h, llm_ctx.v);
            }
        }

        // --- 2. FEED-FORWARD PASS (SwiGLU FFN) ---
        if (llm_weights.layers[layer].ffn_norm && llm_weights.layers[layer].ffn_gate && 
            llm_weights.layers[layer].ffn_up && llm_weights.layers[layer].ffn_down) {
            
            if (!tensor_rmsnorm(llm_ctx.h, llm_ctx.scratch, llm_weights.layers[layer].ffn_norm, float_to_fixed(llm_model.gguf.arch.rms_norm_eps))) {
                klog("LLM error: ffn_norm rmsnorm failed\n");
                return 0;
            }
            
            for (uint32_t d = 0; d < n_embd; d++) {
                h_float[d] = (float)llm_ctx.scratch->data[d] / 65536.0f;
            }

            // Project gate and up
            llama_matmul_fpu(llm_ctx.ffn_gate, llm_ctx.scratch, llm_weights.layers[layer].ffn_gate, h_float);
            llama_matmul_fpu(llm_ctx.ffn_up, llm_ctx.scratch, llm_weights.layers[layer].ffn_up, h_float);

            // Apply SwiGLU: ffn_gate = SiLU(ffn_gate) * ffn_up
            uint32_t n_ff = llm_weights.layers[layer].ffn_gate->dims[0];
            tensor_apply_activation(llm_ctx.ffn_gate, ACT_SILU);
            for (uint32_t d = 0; d < n_ff; d++) {
                llm_ctx.ffn_gate->data[d] = fixed_mul(llm_ctx.ffn_gate->data[d], llm_ctx.ffn_up->data[d]);
            }

            // Project down back to n_embd: llm_ctx.v = matmul(ffn_gate, ffn_down)
            for (uint32_t d = 0; d < n_ff; d++) {
                ffn_gate_float[d] = (float)llm_ctx.ffn_gate->data[d] / 65536.0f;
            }

            llama_matmul_fpu(llm_ctx.v, llm_ctx.ffn_gate, llm_weights.layers[layer].ffn_down, ffn_gate_float);

            // Add residual connection
            tensor_add(llm_ctx.h, llm_ctx.h, llm_ctx.v);
        }
    }

    if (!tensor_rmsnorm(llm_ctx.h, llm_ctx.h, llm_weights.output_norm, float_to_fixed(llm_model.gguf.arch.rms_norm_eps))) {
        klog("LLM error: final output_norm rmsnorm failed\n");
        return 0;
    }
    return 1;
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
    append_str(response, &pos_out, 256, "GGUF Preview: ");

    uint32_t last_tokens[16];
    for (int i = 0; i < 16; i++) last_tokens[i] = 0;

    // Reset sequence context position
    llm_ctx.current_pos = 0;

    // Ingest all prompt tokens except the last one to populate the KV Cache
    for (int p_idx = 0; p_idx < n_tokens - 1; p_idx++) {
        uint32_t p_token = tokens[p_idx];
        if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", p_token, llm_ctx.h->data, n_embd)) {
            copy_cstr_response(response, llm_gguf_runtime_reason_buf);
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }
        if (!run_llama_forward_pass(h_float, ffn_gate_float, n_embd)) {
            copy_cstr_response(response, "LLM: GGUF forward pass failed.");
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }
        llm_ctx.current_pos++;
    }

    uint32_t last_token = tokens[n_tokens - 1];
    int generated_count = 0;
    int max_new_tokens = LLM_GGUF_PREVIEW_MAX_TOKENS;

    uint32_t start_ticks = timer_get_ticks();

    while (generated_count < max_new_tokens) {
        if (!llm_read_gguf_tensor_alias_row(llm_alias_token_embd, "token embedding alias", last_token, llm_ctx.h->data, n_embd)) {
            copy_cstr_response(response, llm_gguf_runtime_reason_buf);
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }

        if (!run_llama_forward_pass(h_float, ffn_gate_float, n_embd)) {
            copy_cstr_response(response, "LLM: GGUF forward pass failed.");
            kfree(ffn_gate_float);
            kfree(h_float);
            return 1;
        }

        uint32_t best_token = 0;
        
        // Cache output tensor info once per inference
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
        uint8_t *output_weight_base = (uint8_t *)(llm_model.start + llm_model.gguf.data_offset + out_tensor_info.offset);

        // Convert current hidden state to float for fast projection
        for (uint32_t d = 0; d < n_embd; d++) {
            h_float[d] = (float)llm_ctx.h->data[d] / 65536.0f;
        }

        uint32_t vocab_limit = LLM_GGUF_PREVIEW_VOCAB_LIMIT; 
        if (llm_model.gguf.arch.n_vocab < vocab_limit) vocab_limit = llm_model.gguf.arch.n_vocab;

        float max_logit_f = -1e30f;
        int found_token = 0;
        for (uint32_t v = LLM_GGUF_FIRST_TEXT_TOKEN; v < vocab_limit; v++) {
            const char *candidate = tokenizer_decode(llm_tokenizer, v);
            extern int sys_abort_requested();
            if (sys_abort_requested()) {
                copy_cstr_response(response, "LLM: Generation aborted by user.");
                kfree(ffn_gate_float);
                kfree(h_float);
                return 1;
            }
            if (timer_get_ticks() - start_ticks > LLM_GGUF_PREVIEW_TIMEOUT_TICKS) {
                copy_cstr_response(response, "LLM: GGUF preview timed out. Use MKLM for fast prompts or load a smaller GGUF.");
                kfree(ffn_gate_float);
                kfree(h_float);
                return 1;
            }

            if (v % 128 == 0 && v > 0) {
                klog("."); // Discrete progress heartbeat
            }
            if (!llm_preview_token_usable(candidate)) {
                continue;
            }

            float logit = 0.0f;
            // FAST PATH: Direct fused dot product using FLOAT math
            if (out_tensor_info.type == GGML_TYPE_Q4_0) {
                uint32_t blocks_per_row = n_embd / 32;
                uint8_t *row_src = output_weight_base + (v * blocks_per_row * 18);
                logit = dot_product_q4_0_float(row_src, h_float, n_embd);
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
            
            // REPETITION PENALTY: Sliding window check (last 16 tokens)
            for (int i = 0; i < 16; i++) {
                if (v == last_tokens[i] && v != 0) {
                    logit -= 40.0f; 
                }
            }

            if (logit > max_logit_f) {
                max_logit_f = logit;
                best_token = v;
                found_token = 1;
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

        const char *word = tokenizer_decode(llm_tokenizer, best_token);
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
        
        append_str(response, &pos_out, 256, word);
        
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
        copy_cstr_response(response, "LLM: GGUF loaded and prompt tokenized, but preview produced no text. Use llm info or loadmodel model.mklm for fast responses.");
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
            return "LLM: GGUF model loaded. Status: GENERATIVE-PREVIEW. Fluid text generation active with KV Cache.\n";
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
            append_str(llm_info_buf, &pos, sizeof(llm_info_buf), llm_backend() == LLM_BACKEND_GGUF_GEN ? "GENERATIVE-PREVIEW" : "PARSER-ONLY");
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
