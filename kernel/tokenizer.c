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
    t->lengths = kmalloc(count * sizeof(uint32_t));
    t->scores = NULL;
    if (!t->tokens || !t->lengths) {
        if (t->tokens) kfree(t->tokens);
        if (t->lengths) kfree(t->lengths);
        kfree(t);
        return NULL;
    }
    memset(t->tokens, 0, count * sizeof(char *));

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
            t->lengths[i] = (uint32_t)len;
        } else {
            tokenizer_free(t);
            return NULL;
        }
        ptr += len;
    }

    /* tokenizer.ggml.scores: one f32 per vocab entry, used as BPE merge
     * priority below (higher score = merged first). Optional -- some GGUF
     * files omit it; tokenizer_encode() falls back to a fixed priority (0
     * for every candidate, so the first mergeable pair found wins) when
     * t->scores is NULL, degrading gracefully instead of failing to load. */
    {
        uint32_t score_count = 0;
        uint8_t *scores_ptr = NULL;
        if (gguf_get_metadata_array(file_start, file_size, "tokenizer.ggml.scores", &score_count, &scores_ptr) &&
            score_count == count && scores_ptr &&
            scores_ptr + (uint64_t)count * 4 <= file_end) {
            float *scores = kmalloc(count * sizeof(float));
            if (scores) {
                memcpy(scores, scores_ptr, count * sizeof(float));
                t->scores = scores;
            }
        }
    }

    /* tokenizer.ggml.token_type: one int32 per vocab entry (GGUF/llama.cpp
     * convention: 1=NORMAL, 2=UNKNOWN, 3=CONTROL, 4=USER_DEFINED, 5=UNUSED,
     * 6=BYTE). CONTROL entries (<s>, </s>, <unk>, ...) are special tokens
     * added directly to the vocab rather than learned through incremental
     * BPE merges, so no amount of pairwise merging in tokenizer_encode() can
     * ever produce them -- they have to be matched as literal substrings
     * before the merge loop runs. Optional, same graceful-degradation
     * pattern as scores above: older/simpler GGUF files (e.g. the
     * llama2.c-style stories15M fixture) that omit this array just get no
     * special-token handling, same behavior as before this field existed.
     */
    t->special_ids = NULL;
    t->special_count = 0;
    {
        uint32_t type_count = 0;
        uint8_t *type_ptr = NULL;
        if (gguf_get_metadata_array(file_start, file_size, "tokenizer.ggml.token_type", &type_count, &type_ptr) &&
            type_count == count && type_ptr &&
            type_ptr + (uint64_t)count * 4 <= file_end) {
            uint32_t *ids = kmalloc(count * sizeof(uint32_t));
            if (ids) {
                uint32_t n = 0;
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t ttype;
                    memcpy(&ttype, type_ptr + (uint32_t)i * 4, 4);
                    if (ttype == 3 /* CONTROL */) {
                        ids[n++] = i;
                    }
                }
                if (n > 0) {
                    t->special_ids = ids;
                    t->special_count = n;
                } else {
                    kfree(ids);
                }
            }
        }
    }

    return t;
}

/**
 * @brief Frees all memory associated with a tokenizer and its vocabulary.
 */
void tokenizer_free(tokenizer_t *t) {
    if (!t) return;
    if (t->tokens) {
        for (uint32_t i = 0; i < t->vocab_size; i++) {
            if (t->tokens[i]) kfree(t->tokens[i]);
        }
        kfree(t->tokens);
    }
    if (t->lengths) kfree(t->lengths);
    if (t->scores) kfree(t->scores);
    if (t->special_ids) kfree(t->special_ids);
    kfree(t);
}

/* Longest exact match among special (CONTROL-type) vocab entries starting
 * at text[0..avail). Longest-match-first avoids a short special token
 * shadowing a longer one that also matches at the same position (not known
 * to happen with today's fixtures, but cheap to get right). Returns the
 * vocab id, or -1 if none match. */
static int tokenizer_find_special_at(tokenizer_t *t, const char *text, uint32_t avail) {
    int best_tok = -1;
    uint32_t best_len = 0;
    for (uint32_t k = 0; k < t->special_count; k++) {
        uint32_t idx = t->special_ids[k];
        uint32_t len = t->lengths[idx];
        if (len == 0 || len > avail) continue;
        if (len <= best_len) continue; /* can't beat current best */
        if (memcmp(t->tokens[idx], text, len) == 0) {
            best_tok = (int)idx;
            best_len = len;
        }
    }
    return best_tok;
}

/* Exact-match vocab lookup by (pointer, length) -- no NUL-termination
 * assumed on the caller's side, since BPE candidates are slices of the
 * encoded prompt buffer, not separately-allocated C strings. Linear scan is
 * fine: called for adjacent-pair candidates of a short prompt during
 * encode, not per output-vocab-token like the generation-time argmax scan
 * in llm.c. */
static int tokenizer_find_exact(tokenizer_t *t, const char *text, uint32_t len) {
    for (uint32_t i = 0; i < t->vocab_size; i++) {
        if (!t->tokens[i]) continue;
        if (t->lengths[i] == len && memcmp(t->tokens[i], text, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* GGUF/SentencePiece vocabularies include a byte-fallback token per
 * possible byte value, named "<0xXX>" with uppercase hex, so any byte that
 * never got merged into a learned piece can still be encoded losslessly
 * instead of silently dropped. */
static int tokenizer_find_byte_fallback(tokenizer_t *t, unsigned char byte) {
    static const char hex[] = "0123456789ABCDEF";
    char name[8];
    name[0] = '<'; name[1] = '0'; name[2] = 'x';
    name[3] = hex[(byte >> 4) & 0xF];
    name[4] = hex[byte & 0xF];
    name[5] = '>'; name[6] = '\0';
    return tokenizer_find_exact(t, name, 6);
}

static int utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; /* invalid lead byte -- treat as a single byte so we still make progress */
}

#define TOKENIZER_MAX_SYMBOLS 512

typedef struct {
    uint32_t offset; /* into the caller's `encoded` buffer */
    uint32_t len;
    int prev;
    int next;
    int forced_token; /* -1, or a vocab id already resolved (special/control
                        * token) that must be emitted as-is and never merged
                        * with a neighboring symbol */
} bpe_symbol_t;

/**
 * @brief Encodes human-readable text into a sequence of token IDs.
 *
 * Implements the SentencePiece/Llama BPE merge algorithm (the one
 * llama.cpp actually runs for tokenizer.ggml.model="llama"): start from one
 * symbol per UTF-8 character, then repeatedly merge the adjacent pair whose
 * concatenation is itself a vocab entry and has the highest
 * tokenizer.ggml.scores priority, until no mergeable pair remains. This
 * replaces a prior "greedy longest full-string match" approximation that
 * never used scores or the model's real merge order at all, so it
 * routinely produced a different token sequence than the model was
 * actually trained on for any word not already a whole vocab entry.
 */
int tokenizer_encode(tokenizer_t *t, const char *text, uint32_t *tokens, uint32_t max_tokens) {
    /* SentencePiece vocabularies (Llama2/TinyStories-style GGUF models) mark
     * a word-leading space with U+2581 "LOWER ONE EIGHTH BLOCK" (UTF-8 bytes
     * E2 96 81) instead of a plain ' ', and llama.cpp/llama2.c always treat
     * the start of a prompt as if preceded by a space. Without this
     * preprocessing, a literal ' ' in the prompt never matches any
     * vocabulary entry (every word-start token begins with the 3-byte
     * marker, not 0x20), so every word-start token would be unreachable --
     * the model never actually sees the prompt it was given. */
    char encoded[600];
    uint32_t epos = 0;

    if (epos + 3 < sizeof(encoded)) {
        encoded[epos++] = (char)0xE2;
        encoded[epos++] = (char)0x96;
        encoded[epos++] = (char)0x81;
    }
    for (uint32_t i = 0; text[i] && epos + 4 < sizeof(encoded); i++) {
        if (text[i] == ' ') {
            encoded[epos++] = (char)0xE2;
            encoded[epos++] = (char)0x96;
            encoded[epos++] = (char)0x81;
        } else {
            encoded[epos++] = text[i];
        }
    }
    encoded[epos] = '\0';

    if (epos == 0) return 0;

    bpe_symbol_t symbols[TOKENIZER_MAX_SYMBOLS];
    int n_symbols = 0;
    uint32_t pos = 0;
    while (pos < epos && n_symbols < TOKENIZER_MAX_SYMBOLS) {
        int forced = t->special_count ? tokenizer_find_special_at(t, encoded + pos, epos - pos) : -1;
        int clen;
        if (forced >= 0) {
            clen = (int)t->lengths[forced];
        } else {
            clen = utf8_char_len((unsigned char)encoded[pos]);
            if (pos + (uint32_t)clen > epos) clen = (int)(epos - pos);
        }
        symbols[n_symbols].offset = pos;
        symbols[n_symbols].len = (uint32_t)clen;
        symbols[n_symbols].prev = n_symbols - 1;
        symbols[n_symbols].next = n_symbols + 1;
        symbols[n_symbols].forced_token = forced;
        pos += (uint32_t)clen;
        n_symbols++;
    }
    symbols[n_symbols - 1].next = -1;

    /* Repeatedly merge the best-scoring adjacent pair that forms a known
     * vocab entry. n_symbols is small (bounded by prompt length), so
     * rescanning all active pairs each round is simple and fast enough --
     * this runs once per prompt, not once per generated token. Symbols
     * already resolved to a special/control token (forced_token >= 0) are
     * skipped as merge candidates: they were never reached through
     * pairwise merges during training, so merging them with a neighbor
     * would only ever produce a nonexistent or wrong vocab entry. */
    while (1) {
        int best_i = -1;
        float best_score = 0.0f;
        int found_any = 0;

        for (int i = 0; i != -1; i = symbols[i].next) {
            int j = symbols[i].next;
            if (j == -1) break;
            if (symbols[i].forced_token >= 0 || symbols[j].forced_token >= 0) continue;
            uint32_t merged_len = symbols[i].len + symbols[j].len;
            int tok = tokenizer_find_exact(t, encoded + symbols[i].offset, merged_len);
            if (tok < 0) continue;
            float score = t->scores ? t->scores[tok] : 0.0f;
            if (!found_any || score > best_score) {
                found_any = 1;
                best_score = score;
                best_i = i;
            }
        }

        if (!found_any) break;

        int j = symbols[best_i].next;
        symbols[best_i].len += symbols[j].len;
        symbols[best_i].next = symbols[j].next;
        if (symbols[j].next != -1) {
            symbols[symbols[j].next].prev = best_i;
        }
    }

    /* Emit one token per final symbol: the pre-resolved special token if
     * this symbol came from tokenizer_find_special_at(), otherwise an exact
     * vocab match if the merge loop above found one (it always did for
     * anything merged past a single character), otherwise fall back to
     * per-byte <0xXX> tokens instead of silently dropping the character. */
    uint32_t token_count = 0;
    for (int i = 0; i != -1 && token_count < max_tokens; i = symbols[i].next) {
        if (symbols[i].forced_token >= 0) {
            tokens[token_count++] = (uint32_t)symbols[i].forced_token;
            continue;
        }
        const char *sym_text = encoded + symbols[i].offset;
        uint32_t sym_len = symbols[i].len;
        int tok = tokenizer_find_exact(t, sym_text, sym_len);
        if (tok >= 0) {
            tokens[token_count++] = (uint32_t)tok;
            continue;
        }
        for (uint32_t b = 0; b < sym_len && token_count < max_tokens; b++) {
            int btok = tokenizer_find_byte_fallback(t, (unsigned char)sym_text[b]);
            if (btok >= 0) {
                tokens[token_count++] = (uint32_t)btok;
            }
        }
    }

    return (int)token_count;
}

/**
 * @brief Decodes a single token ID back into its string representation.
 */
const char *tokenizer_decode(tokenizer_t *t, uint32_t token_id) {
    if (token_id < t->vocab_size) return t->tokens[token_id];
    return "[UNK]";
}
