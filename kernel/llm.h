#ifndef LLM_H
#define LLM_H

#include <stdint.h>

/*
 * LLM Engine Server Interface
 */
void llm_server_init();
void llm_server_process();
void llm_load_model_module(uint32_t start, uint32_t end, const char *name);
int llm_load_model_file(const char *name);
int llm_model_loaded();
int llm_model_valid();
int llm_model_supported();
uint32_t llm_model_size();
const char *llm_model_name();
const char *llm_status_string();
const char *llm_info_string();
const char *llm_test_string();
void llm_set_trace(int enabled);
int llm_trace_enabled(void);
void llm_inference(const char *prompt, char *response);
void sys_llm_query(const char *prompt, char *response);
/* Returns 1=PASS (tokens generated), -1=FAIL (0 tokens), 0=SKIP (not GGUF-GEN) */
int llm_gguf_selftest(void);
/* Diagnostic: dumps hidden-state energy + top-5 output logits for the token
 * right after `prompt` to the kernel log, instead of generating text. */
void llm_dump_logits(const char *prompt, char *response);

#endif
