#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stdint.h>

typedef struct {
    uint32_t vocab_size;
    char **tokens;
    uint32_t *lengths; /* cached strlen(tokens[i]), avoids recomputing it on every BPE merge-candidate lookup */
    float *scores;
} tokenizer_t;

tokenizer_t *tokenizer_create(uint32_t file_start, uint32_t file_size);
void tokenizer_free(tokenizer_t *t);

int tokenizer_encode(tokenizer_t *t, const char *text, uint32_t *tokens, uint32_t max_tokens);
const char *tokenizer_decode(tokenizer_t *t, uint32_t token_id);

#endif
