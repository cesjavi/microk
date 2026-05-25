#include "tokenizer.h"
#include "llm_gguf.h"
#include "kheap.h"
#include <string.h>

/**
 * @brief Initializes a tokenizer by extracting vocabulary from a GGUF file.
 */
tokenizer_t *tokenizer_create(uint32_t file_start, uint32_t file_size) {
    uint32_t count = 0;
    uint8_t *tokens_ptr = NULL;
    uint8_t *file_end = (uint8_t *)(file_start + file_size);
    
    if (!gguf_get_metadata_array(file_start, file_size, "tokenizer.ggml.tokens", &count, &tokens_ptr)) {
        return NULL;
    }
    if (count == 0 || count > 65536 || !tokens_ptr || tokens_ptr >= file_end) {
        return NULL;
    }

    tokenizer_t *t = kmalloc(sizeof(tokenizer_t));
    if (!t) return NULL;
    
    t->vocab_size = count;
    t->tokens = kmalloc(count * sizeof(char *));
    if (!t->tokens) {
        kfree(t);
        return NULL;
    }
    memset(t->tokens, 0, count * sizeof(char *));
    t->scores = NULL; // Optional, can load if needed

    uint8_t *ptr = tokens_ptr;
    for (uint32_t i = 0; i < count; i++) {
        if (ptr + 8 > file_end) {
            tokenizer_free(t);
            return NULL;
        }
        uint64_t len = *(uint64_t *)ptr;
        if (len > 0xFFFFFFFFULL) {
            tokenizer_free(t);
            return NULL;
        }
        ptr += 8;
        if (ptr + len > file_end) {
            tokenizer_free(t);
            return NULL;
        }
        
        t->tokens[i] = kmalloc((uint32_t)len + 1);
        if (t->tokens[i]) {
            memcpy(t->tokens[i], ptr, (uint32_t)len);
            t->tokens[i][len] = '\0';
        } else {
            tokenizer_free(t);
            return NULL;
        }
        ptr += len;
    }

    return t;
}

/**
 * @brief Frees all memory associated with a tokenizer and its vocabulary.
 */
void tokenizer_free(tokenizer_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->vocab_size; i++) {
        if (t->tokens[i]) kfree(t->tokens[i]);
    }
    kfree(t->tokens);
    kfree(t);
}

/**
 * @brief Encodes human-readable text into a sequence of token IDs.
 * Uses a greedy longest-match algorithm against the vocabulary.
 */
int tokenizer_encode(tokenizer_t *t, const char *text, uint32_t *tokens, uint32_t max_tokens) {
    uint32_t token_count = 0;
    const char *ptr = text;
    
    while (*ptr && token_count < max_tokens) {
        int best_token = -1;
        int best_len = 0;
        
        for (uint32_t i = 0; i < t->vocab_size; i++) {
            if (!t->tokens[i]) continue;
            int len = strlen(t->tokens[i]);
            if (len > best_len && strncmp(ptr, t->tokens[i], len) == 0) {
                best_len = len;
                best_token = i;
            }
        }
        
        if (best_token != -1) {
            tokens[token_count++] = best_token;
            ptr += best_len;
        } else {
            ptr++; // Skip unknown char
        }
    }
    
    return token_count;
}

/**
 * @brief Decodes a single token ID back into its string representation.
 */
const char *tokenizer_decode(tokenizer_t *t, uint32_t token_id) {
    if (token_id < t->vocab_size) return t->tokens[token_id];
    return "[UNK]";
}
