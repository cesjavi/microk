#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stdint.h>

typedef struct {
    uint32_t vocab_size;
    char **tokens;
    uint32_t *lengths; /* cached strlen(tokens[i]), avoids recomputing it on every BPE merge-candidate lookup */
    float *scores;
    uint32_t *special_ids; /* vocab indices with tokenizer.ggml.token_type == CONTROL (e.g. <s>, </s>, <unk>) --
                             * matched as atomic substrings before BPE merging, since these were never reached
                             * by any chain of pairwise merges during training and so can never be reconstructed
                             * by the merge loop itself (NULL/0 if the GGUF has no token_type array). */
    uint32_t special_count;
} tokenizer_t;

tokenizer_t *tokenizer_create(uint32_t file_start, uint32_t file_size);
void tokenizer_free(tokenizer_t *t);

int tokenizer_encode(tokenizer_t *t, const char *text, uint32_t *tokens, uint32_t max_tokens);
const char *tokenizer_decode(tokenizer_t *t, uint32_t token_id);

#endif
